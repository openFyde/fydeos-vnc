// Copyright 2026 The FydeOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FYDEOS_VNC_MOJO_CHROME_CLIENT_H_
#define FYDEOS_VNC_MOJO_CHROME_CLIENT_H_

#include <memory>

#include <base/memory/scoped_refptr.h>
#include <base/task/single_thread_task_runner.h>
#include <dbus/bus.h>
#include <mojo/core/embedder/scoped_ipc_support.h>
#include <mojo/public/cpp/bindings/remote.h>

#include "mojo/fydeos_vnc.mojom.h"

namespace fydeos::vnc::mojo_client {

// Initializes the Mojo runtime and brokers a connection to Chrome's
// FydeosVncService via the system MojoConnectionService D-Bus method
// (BootstrapMojoConnectionForFydeosVnc). Must be called from the daemon's
// main thread before any Mojo call.
//
// Returns a bound Remote on success, an unbound Remote if Chrome is not
// reachable. Caller should retry later in that case.
class ChromeClient {
 public:
  ChromeClient();
  ChromeClient(const ChromeClient&) = delete;
  ChromeClient& operator=(const ChromeClient&) = delete;
  ~ChromeClient();

  // Brokers the bootstrap. |bus| must already be connected to the system bus.
  bool Bootstrap(dbus::Bus* bus);

  ::fydeos::vnc::mojom::FydeosVncService* service() {
    return service_.get();
  }

  // Convenience accessors that lazily Bind sub-interfaces.
  ::fydeos::vnc::mojom::FrameSinkCaptureService* frame_sink();
  ::fydeos::vnc::mojom::InputInjectionService* input_injection();

 private:
  std::unique_ptr<::mojo::core::ScopedIPCSupport> ipc_support_;
  ::mojo::Remote<::fydeos::vnc::mojom::FydeosVncService> service_;
  ::mojo::Remote<::fydeos::vnc::mojom::FrameSinkCaptureService> frame_sink_;
  ::mojo::Remote<::fydeos::vnc::mojom::InputInjectionService> input_injection_;
};

}  // namespace fydeos::vnc::mojo_client

#endif  // FYDEOS_VNC_MOJO_CHROME_CLIENT_H_
