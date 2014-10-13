// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CONTENT_COMMON_CHILD_PROCESS_HOST_IMPL_H_
#define CONTENT_COMMON_CHILD_PROCESS_HOST_IMPL_H_

#include <string>
#include <vector>

#include "build/build_config.h"

#include "base/basictypes.h"
#include "base/memory/scoped_ptr.h"
#include "base/memory/shared_memory.h"
#include "base/memory/singleton.h"
#include "base/strings/string16.h"
#include "content/public/common/child_process_host.h"
#include "ipc/ipc_listener.h"
#include "ui/gfx/gpu_memory_buffer.h"

namespace base {
class FilePath;
}

namespace IPC {
class MessageFilter;
}

namespace content {
class ChildProcessHostDelegate;

// Provides common functionality for hosting a child process and processing IPC
// messages between the host and the child process. Users are responsible
// for the actual launching and terminating of the child processes.
class CONTENT_EXPORT ChildProcessHostImpl : public ChildProcessHost,
                                            public IPC::Listener {
 public:
  virtual ~ChildProcessHostImpl();

  // Public and static for reuse by RenderMessageFilter.
  static void AllocateSharedMemory(
      size_t buffer_size, base::ProcessHandle child_process,
      base::SharedMemoryHandle* handle);

  // Returns a unique ID to identify a child process. On construction, this
  // function will be used to generate the id_, but it is also used to generate
  // IDs for the RenderProcessHost, which doesn't inherit from us, and whose IDs
  // must be unique for all child processes.
  //
  // This function is threadsafe since RenderProcessHost is on the UI thread,
  // but normally this will be used on the IO thread.
  //
  // This will never return ChildProcessHost::kInvalidUniqueID.
  static int GenerateChildProcessUniqueId();

  // ChildProcessHost implementation
  virtual bool Send(IPC::Message* message) override;
  virtual void ForceShutdown() override;
  virtual std::string CreateChannel() override;
  virtual bool IsChannelOpening() override;
  virtual void AddFilter(IPC::MessageFilter* filter) override;
#if defined(OS_POSIX)
  virtual int TakeClientFileDescriptor() override;
#endif

 private:
  friend class ChildProcessHost;

  explicit ChildProcessHostImpl(ChildProcessHostDelegate* delegate);

  // IPC::Listener methods:
  virtual bool OnMessageReceived(const IPC::Message& msg) override;
  virtual void OnChannelConnected(int32 peer_pid) override;
  virtual void OnChannelError() override;
  virtual void OnBadMessageReceived(const IPC::Message& message) override;

  // Message handlers:
  void OnShutdownRequest();
  void OnAllocateSharedMemory(uint32 buffer_size,
                              base::SharedMemoryHandle* handle);
  void OnAllocateGpuMemoryBuffer(uint32 width,
                                 uint32 height,
                                 gfx::GpuMemoryBuffer::Format format,
                                 gfx::GpuMemoryBuffer::Usage usage,
                                 IPC::Message* reply);
  void OnDeletedGpuMemoryBuffer(gfx::GpuMemoryBufferType type,
                                const gfx::GpuMemoryBufferId& id);

  void GpuMemoryBufferAllocated(IPC::Message* reply,
                                const gfx::GpuMemoryBufferHandle& handle);

  ChildProcessHostDelegate* delegate_;
  base::ProcessHandle peer_handle_;
  bool opening_channel_;  // True while we're waiting the channel to be opened.
  scoped_ptr<IPC::Channel> channel_;
  std::string channel_id_;

  // Holds all the IPC message filters.  Since this object lives on the IO
  // thread, we don't have a IPC::ChannelProxy and so we manage filters
  // manually.
  std::vector<scoped_refptr<IPC::MessageFilter> > filters_;

  DISALLOW_COPY_AND_ASSIGN(ChildProcessHostImpl);
};

}  // namespace content

#endif  // CONTENT_COMMON_CHILD_PROCESS_HOST_IMPL_H_
