/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set sw=2 ts=8 et tw=80 : */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/layers/SharedBufferManagerParent.h"
#include "base/message_loop.h"          // for MessageLoop
#include "base/process.h"               // for ProcessId
#include "base/task.h"                  // for CancelableTask, DeleteTask, etc
#include "base/thread.h"
#include "mozilla/ipc/MessageChannel.h" // for MessageChannel, etc
#include "mozilla/ipc/ProtocolUtils.h"
#include "mozilla/ipc/Transport.h"      // for Transport
#include "mozilla/UniquePtr.h"          // for UniquePtr
#include "mozilla/Unused.h"
#include "nsIMemoryReporter.h"
#ifdef MOZ_WIDGET_GONK
#include "ui/PixelFormat.h"
#include "android/hardware_buffer.h"
#endif
#include "nsPrintfCString.h"
#include "nsThreadUtils.h"
#include <dlfcn.h>

using namespace mozilla::ipc;
#ifdef MOZ_WIDGET_GONK
using namespace android;
#endif
using std::map;

namespace mozilla {
namespace layers {

map<base::ProcessId, SharedBufferManagerParent* > SharedBufferManagerParent::sManagers;
StaticAutoPtr<Monitor> SharedBufferManagerParent::sManagerMonitor;
uint64_t SharedBufferManagerParent::sBufferKey(0);

// FIXME
#if 0
#ifdef MOZ_WIDGET_GONK
class GrallocReporter final : public nsIMemoryReporter
{
public:
  NS_DECL_ISUPPORTS

  NS_IMETHOD CollectReports(nsIHandleReportCallback* aHandleReport,
                            nsISupports* aData, bool aAnonymize)
  {
    if (SharedBufferManagerParent::sManagerMonitor) {
      SharedBufferManagerParent::sManagerMonitor->Lock();
    }
    map<base::ProcessId, SharedBufferManagerParent*>::iterator it;
    for (it = SharedBufferManagerParent::sManagers.begin(); it != SharedBufferManagerParent::sManagers.end(); it++) {
      base::ProcessId pid = it->first;
      SharedBufferManagerParent *mgr = it->second;
      if (!mgr) {
        printf_stderr("GrallocReporter::CollectReports() mgr is nullptr");
        continue;
      }

      nsAutoCString pidName;
      LinuxUtils::GetThreadName(pid, pidName);

      MutexAutoLock lock(mgr->mLock);
      std::map<int64_t, android::sp<android::GraphicBuffer> >::iterator buf_it;
      for (buf_it = mgr->mBuffers.begin(); buf_it != mgr->mBuffers.end(); buf_it++) {
        nsresult rv;
        android::sp<android::GraphicBuffer> gb = buf_it->second;
        int bpp = android::bytesPerPixel(gb->getPixelFormat());
        int stride = gb->getStride();
        int height = gb->getHeight();
        int amount = bpp > 0
          ? (stride * height * bpp)
          // Special case for BSP specific formats (mainly YUV formats, count it as normal YUV buffer).
          : (stride * height * 3 / 2);

        nsPrintfCString gpath("gralloc/%s (pid=%d)/buffer(width=%d, height=%d, bpp=%d, stride=%d)",
            pidName.get(), pid, gb->getWidth(), height, bpp, stride);

        rv = aHandleReport->Callback(EmptyCString(), gpath, KIND_OTHER, UNITS_BYTES, amount,
            NS_LITERAL_CSTRING(
              "Special RAM that can be shared between processes and directly accessed by "
              "both the CPU and GPU. Gralloc memory is usually a relatively precious "
              "resource, with much less available than generic RAM. When it's exhausted, "
              "graphics performance can suffer. This value can be incorrect because of race "
              "conditions."),
            aData);
        if (rv != NS_OK) {
          if (SharedBufferManagerParent::sManagerMonitor) {
            SharedBufferManagerParent::sManagerMonitor->Unlock();
          }
          return rv;
        }
      }
    }
    if (SharedBufferManagerParent::sManagerMonitor) {
      SharedBufferManagerParent::sManagerMonitor->Unlock();
    }
    return NS_OK;
  }

protected:
  ~GrallocReporter() {}
};

NS_IMPL_ISUPPORTS(GrallocReporter, nsIMemoryReporter)
#endif

void InitGralloc() {
  NS_ASSERTION(NS_IsMainThread(), "Should be on main thread.");
#ifdef MOZ_WIDGET_GONK
  RegisterStrongMemoryReporter(new GrallocReporter());
#endif
}
#endif

/**
 * Task that deletes SharedBufferManagerParent on a specified thread.
 */
class DeleteSharedBufferManagerParentTask : public Runnable
{
public:
    explicit DeleteSharedBufferManagerParentTask(RefPtr<SharedBufferManagerParent> aSharedBufferManager)
        : Runnable("DeleteSharedBufferManagerParentTask")
        , mSharedBufferManager(aSharedBufferManager) {
    }
    NS_IMETHOD Run() override { return NS_OK; }
private:
    RefPtr<SharedBufferManagerParent> mSharedBufferManager;
};

/* static */
SharedBufferManagerParent* SharedBufferManagerParent::CreateSameProcess() {
  base::ProcessId pid = base::GetCurrentProcId();

  char thrname[128];
  base::snprintf(thrname, 128, "BufMgrParent#%d", pid);

  RefPtr<SharedBufferManagerParent> parent =
      new SharedBufferManagerParent(pid, new base::Thread(thrname));
  parent->mSelfRef = parent;
  return parent;
}

/* static */
bool SharedBufferManagerParent::CreateForContent(
    Endpoint<PSharedBufferManagerParent>&& aEndpoint) {
  base::Thread* thread = nullptr;
  char thrname[128];
  base::snprintf(thrname, 128, "BufMgrParent#%d", aEndpoint.OtherPid());
  thread = new base::Thread(thrname);

  RefPtr<SharedBufferManagerParent> bridge =
      new SharedBufferManagerParent(aEndpoint.OtherPid(), thread);
  MOZ_ASSERT(thread->IsRunning());
  thread->message_loop()->PostTask(NewRunnableMethod<Endpoint<PSharedBufferManagerParent>&&>(
      "layers::SharedBufferManagerParent::Bind", bridge, &SharedBufferManagerParent::Bind,
      std::move(aEndpoint)));

  return true;
}

void SharedBufferManagerParent::Bind(Endpoint<PSharedBufferManagerParent>&& aEndpoint) {
  if (!aEndpoint.Bind(this)) return;
  mSelfRef = this;
}

SharedBufferManagerParent::SharedBufferManagerParent(base::ProcessId aOwner, base::Thread* aThread)
  : mThread(aThread)
  , mMainMessageLoop(MessageLoop::current())
  , mDestroyed(false)
  , mLock("SharedBufferManagerParent.mLock")
{
  MOZ_ASSERT(NS_IsMainThread());
  if (!sManagerMonitor) {
    sManagerMonitor = new Monitor("Manager Monitor");
  }

  MonitorAutoLock lock(*sManagerMonitor.get());
  NS_ASSERTION(NS_IsMainThread(), "Should be on main thread");
  if (!aThread->IsRunning()) {
    aThread->Start();
  }

  if (sManagers.count(aOwner) != 0) {
    printf_stderr("SharedBufferManagerParent already exists.");
  }
  mOwner = aOwner;
  sManagers[aOwner] = this;
}

SharedBufferManagerParent::~SharedBufferManagerParent()
{
  MonitorAutoLock lock(*sManagerMonitor.get());
  sManagers.erase(mOwner);
  delete mThread;
}

void
SharedBufferManagerParent::ActorDestroy(ActorDestroyReason aWhy)
{
  MutexAutoLock lock(mLock);
  mDestroyed = true;
#ifdef MOZ_HAVE_SURFACEDESCRIPTORGRALLOC
  mBuffers.clear();
#endif
  RefPtr<Runnable> task = new DeleteSharedBufferManagerParentTask(this);
  mSelfRef = nullptr;
  mMainMessageLoop->PostTask(task.forget());
}

bool SharedBufferManagerParent::RecvAllocateGrallocBuffer(const IntSize& aSize, const uint32_t& aFormat, const uint32_t& aUsage, mozilla::layers::MaybeMagicGrallocBufferHandle* aHandle)
{
#ifdef MOZ_HAVE_SURFACEDESCRIPTORGRALLOC

  *aHandle = null_t();

  if (aFormat == 0 || aUsage == 0) {
    printf_stderr("SharedBufferManagerParent::RecvAllocateGrallocBuffer -- format and usage must be non-zero");
    return false;
  }

  if (aSize.width <= 0 || aSize.height <= 0) {
    printf_stderr("SharedBufferManagerParent::RecvAllocateGrallocBuffer -- requested gralloc buffer size is invalid");
    return false;
  }

  // If the requested size is too big (i.e. exceeds the commonly used max GL texture size)
  // then we risk OOMing the parent process. It's better to just deny the allocation and
  // kill the child process, which is what the following code does.
  // TODO: actually use GL_MAX_TEXTURE_SIZE instead of hardcoding 4096
  if (aSize.width > 4096 || aSize.height > 4096) {
    printf_stderr("SharedBufferManagerParent::RecvAllocateGrallocBuffer -- requested gralloc buffer is too big.");
    return false;
  }

#if ANDROID_VERSION >= 27
  typedef int (*fnAHardwareBuffer_allocate)(const AHardwareBuffer_Desc* desc, AHardwareBuffer** outBuffer);
  typedef void (*fnAHardwareBuffer_describe)(const AHardwareBuffer* buffer,
                                                AHardwareBuffer_Desc* outDesc);
  void* lib = dlopen("/system/lib/libandroid.so", RTLD_NOW);
  if (lib == NULL) {
    printf_stderr("Could not dlopen(\"libandroid.so\"):");
  } else {
    fnAHardwareBuffer_allocate fAHardwareBuffer_allocate = (fnAHardwareBuffer_allocate) dlsym(lib, "AHardwareBuffer_allocate") ;
    fnAHardwareBuffer_describe fAHardwareBuffer_describe = (fnAHardwareBuffer_describe) dlsym(lib, "AHardwareBuffer_describe") ;
    if (fAHardwareBuffer_allocate == NULL || fAHardwareBuffer_describe == NULL) {
      printf_stderr("Symbol 'AHardwareBuffer_allocate' is missing from shared library!!\n");
    } else {
      AHardwareBuffer* graphicBuf;
      AHardwareBuffer_Desc usage;

      // filling in the usage for HardwareBuffer
      usage.format = aFormat;
      usage.height = aSize.height;
      usage.width = aSize.width;
      usage.layers = 1;
      usage.rfu0 = 0;
      usage.rfu1 = 0;
      usage.stride = 10;
      usage.usage = aUsage;

      fAHardwareBuffer_allocate(&usage, &graphicBuf);
      // ACTUAL parameters of the AHardwareBuffer which it reports
      AHardwareBuffer_Desc usage1;

      fAHardwareBuffer_describe(graphicBuf, &usage1);
      sp<GraphicBuffer> outgoingBuffer = reinterpret_cast< GraphicBuffer*>(graphicBuf);
#else
// for old ANDROID_VERSION, use original GraphicBuffer
      sp<GraphicBuffer> outgoingBuffer = new GraphicBuffer(aSize.width, aSize.height, aFormat, aUsage);
#endif
      if (!outgoingBuffer.get() || outgoingBuffer->initCheck() != NO_ERROR) {
        printf_stderr("SharedBufferManagerParent::RecvAllocateGrallocBuffer -- gralloc buffer allocation failed");
        return true;
      }

      int64_t bufferKey;
      {
        MonitorAutoLock lock(*sManagerMonitor.get());
        bufferKey = ++sBufferKey;
      }
      GrallocBufferRef ref;
      ref.mOwner = mOwner;
      ref.mKey = bufferKey;
      *aHandle = MagicGrallocBufferHandle(outgoingBuffer, ref);

      {
        MutexAutoLock lock(mLock);
        mBuffers[bufferKey] = outgoingBuffer;
      }
#if ANDROID_VERSION >= 27
    }
  }
#endif

#endif
  return true;
}

bool SharedBufferManagerParent::RecvDropGrallocBuffer(const mozilla::layers::MaybeMagicGrallocBufferHandle& handle)
{
#ifdef MOZ_HAVE_SURFACEDESCRIPTORGRALLOC
  NS_ASSERTION(handle.type() == MaybeMagicGrallocBufferHandle::TGrallocBufferRef, "We shouldn't interact with the real buffer!");
  int64_t bufferKey = handle.get_GrallocBufferRef().mKey;
  sp<GraphicBuffer> buf = GetGraphicBuffer(bufferKey);
  MOZ_ASSERT(buf.get());
  MutexAutoLock lock(mLock);
  NS_ASSERTION(mBuffers.count(bufferKey) == 1, "No such buffer");
  mBuffers.erase(bufferKey);

  if(!buf.get()) {
    printf_stderr("SharedBufferManagerParent::RecvDropGrallocBuffer -- invalid buffer key.");
    return true;
  }

#endif
  return true;
}

void SharedBufferManagerParent::DropGrallocBufferSync(mozilla::layers::SurfaceDescriptor aDesc)
{
  DropGrallocBufferImpl(aDesc);
}

/*static*/
void SharedBufferManagerParent::DropGrallocBuffer(ProcessId id, mozilla::layers::SurfaceDescriptor aDesc)
{
  if (aDesc.type() != SurfaceDescriptor::TSurfaceDescriptorGralloc) {
    return;
  }

  MonitorAutoLock lock(*sManagerMonitor.get());
  SharedBufferManagerParent* mgr = SharedBufferManagerParent::GetInstance(id);
  if (!mgr) {
    return;
  }

  MutexAutoLock mgrlock(mgr->mLock);
  if (mgr->mDestroyed) {
    return;
  }

  if (PlatformThread::CurrentId() == mgr->mThread->thread_id()) {
    MOZ_CRASH("GFX: SharedBufferManagerParent::DropGrallocBuffer should not be called on SharedBufferManagerParent thread");
  } else {
    RefPtr<Runnable> runnable =
        WrapRunnable(RefPtr<SharedBufferManagerParent>(mgr),
                     &SharedBufferManagerParent::DropGrallocBufferSync, aDesc);
  }
  return;
}

void SharedBufferManagerParent::DropGrallocBufferImpl(mozilla::layers::SurfaceDescriptor aDesc)
{
  MutexAutoLock lock(mLock);
  if (mDestroyed) {
    return;
  }
#ifdef MOZ_HAVE_SURFACEDESCRIPTORGRALLOC
  int64_t key = -1;
  MaybeMagicGrallocBufferHandle handle;
  if (aDesc.type() == SurfaceDescriptor::TSurfaceDescriptorGralloc) {
    handle = aDesc.get_SurfaceDescriptorGralloc().buffer();
  } else {
    return;
  }

  if (handle.type() == MaybeMagicGrallocBufferHandle::TGrallocBufferRef) {
    key = handle.get_GrallocBufferRef().mKey;
  } else if (handle.type() == MaybeMagicGrallocBufferHandle::TMagicGrallocBufferHandle) {
    key = handle.get_MagicGrallocBufferHandle().mRef.mKey;
  }

  NS_ASSERTION(key != -1, "Invalid buffer key");
  NS_ASSERTION(mBuffers.count(key) == 1, "No such buffer");
  mBuffers.erase(key);
  mozilla::Unused << SendDropGrallocBuffer(handle);
#endif
}

MessageLoop* SharedBufferManagerParent::GetMessageLoop()
{
  return mThread->message_loop();
}

SharedBufferManagerParent* SharedBufferManagerParent::GetInstance(ProcessId id)
{
  NS_ASSERTION(sManagers.count(id) == 1, "No BufferManager for the process");
  if (sManagers.count(id) == 1) {
    return sManagers[id];
  } else {
    return nullptr;
  }
}

#ifdef MOZ_HAVE_SURFACEDESCRIPTORGRALLOC
android::sp<android::GraphicBuffer>
SharedBufferManagerParent::GetGraphicBuffer(int64_t key)
{
  MutexAutoLock lock(mLock);
  if (mBuffers.count(key) == 1) {
    return mBuffers[key];
  } else {
    // The buffer can be dropped, or invalid
    printf_stderr("SharedBufferManagerParent::GetGraphicBuffer -- invalid key");
    return nullptr;
  }
}

android::sp<android::GraphicBuffer>
SharedBufferManagerParent::GetGraphicBuffer(GrallocBufferRef aRef)
{
  MonitorAutoLock lock(*sManagerMonitor.get());
  SharedBufferManagerParent* parent = GetInstance(aRef.mOwner);
  if (!parent) {
    return nullptr;
  }
  return parent->GetGraphicBuffer(aRef.mKey);
}
#endif

} /* namespace layers */
} /* namespace mozilla */
