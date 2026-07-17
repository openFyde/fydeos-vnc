// Copyright 2026 The FydeOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "fydeos-vnc/mojo/chrome_client.h"

#include <utility>

#include <base/files/file_util.h>
#include <base/files/scoped_file.h>
#include <base/logging.h>
#include <chromeos/dbus/service_constants.h>
#include <dbus/message.h>
#include <dbus/object_path.h>
#include <dbus/object_proxy.h>
#include <mojo/core/embedder/embedder.h>
#include <mojo/public/cpp/bindings/pending_receiver.h>
#include <mojo/public/cpp/bindings/pending_remote.h>
#include <mojo/public/cpp/platform/platform_channel_endpoint.h>
#include <mojo/public/cpp/platform/platform_handle.h>
#include <mojo/public/cpp/system/invitation.h>

namespace fydeos::vnc::mojo_client {

namespace {

bool g_mojo_initialized = false;

void EnsureMojoInit() {
  if (g_mojo_initialized) {
    return;
  }
  ::mojo::core::Init();
  g_mojo_initialized = true;
}

}  // namespace

ChromeClient::ChromeClient() = default;
ChromeClient::~ChromeClient() = default;

bool ChromeClient::Bootstrap(dbus::Bus* bus) {
  bus_ = bus;
  if (service_.is_bound()) {
    return true;
  }
  EnsureMojoInit();
  if (!ipc_support_) {
    ipc_support_ = std::make_unique<::mojo::core::ScopedIPCSupport>(
        ::base::SingleThreadTaskRunner::GetCurrentDefault(),
        ::mojo::core::ScopedIPCSupport::ShutdownPolicy::FAST);
  }

  dbus::ObjectProxy* proxy = bus->GetObjectProxy(
      ::mojo_connection_service::kMojoConnectionServiceServiceName,
      dbus::ObjectPath(
          ::mojo_connection_service::kMojoConnectionServiceServicePath));
  if (!proxy) {
    LOG(ERROR) << "MojoConnectionService proxy unavailable";
    return false;
  }

  dbus::MethodCall call(
      ::mojo_connection_service::kMojoConnectionServiceInterface,
      ::mojo_connection_service::kBootstrapMojoConnectionForFydeosVncMethod);

  auto resp = proxy->CallMethodAndBlock(&call, /*timeout_ms=*/25000)
                  .value_or(nullptr);
  if (!resp) {
    LOG(WARNING) << "Bootstrap call to Chrome returned no response (Chrome "
                    "not up?)";
    return false;
  }

  base::ScopedFD fd;
  dbus::MessageReader reader(resp.get());
  if (!reader.PopFileDescriptor(&fd) || !fd.is_valid()) {
    LOG(ERROR) << "Bootstrap response missing valid fd";
    return false;
  }
  if (!base::SetCloseOnExec(fd.get())) {
    PLOG(WARNING) << "SetCloseOnExec on mojo fd";
  }

#if defined(ENABLE_IPCZ_ON_CHROMEOS)
  ::mojo::IncomingInvitation invitation = ::mojo::IncomingInvitation::Accept(
      ::mojo::PlatformChannelEndpoint(
          ::mojo::PlatformHandle(std::move(fd))),
      MOJO_ACCEPT_INVITATION_FLAG_INHERIT_BROKER);
#else
  ::mojo::IncomingInvitation invitation = ::mojo::IncomingInvitation::Accept(
      ::mojo::PlatformChannelEndpoint(
          ::mojo::PlatformHandle(std::move(fd))));
#endif

  service_.Bind(::mojo::PendingRemote<::fydeos::vnc::mojom::FydeosVncService>(
      invitation.ExtractMessagePipe(0), /*version=*/0u));
  service_.reset_on_disconnect();
  LOG(INFO) << "Bootstrapped Mojo connection to Chrome FydeosVncService";
  return service_.is_bound();
}

bool ChromeClient::EnsureConnected() {
  if (service_.is_bound()) {
    return true;
  }
  // Chrome went away (logout restarts it); the sub-interface remotes were
  // reset by reset_on_disconnect and will lazily rebind on next access
  // once the service pipe is up again.
  if (!bus_) {
    return false;
  }
  return Bootstrap(bus_.get());
}

::fydeos::vnc::mojom::FrameSinkCaptureService* ChromeClient::frame_sink() {
  if (!service_.is_bound()) {
    return nullptr;
  }
  if (!frame_sink_.is_bound()) {
    service_->BindFrameSinkCaptureService(
        frame_sink_.BindNewPipeAndPassReceiver());
    frame_sink_.reset_on_disconnect();
  }
  return frame_sink_.get();
}

::fydeos::vnc::mojom::InputInjectionService* ChromeClient::input_injection() {
  if (!service_.is_bound()) {
    return nullptr;
  }
  if (!input_injection_.is_bound()) {
    service_->BindInputInjectionService(
        input_injection_.BindNewPipeAndPassReceiver());
    input_injection_.reset_on_disconnect();
  }
  return input_injection_.get();
}

}  // namespace fydeos::vnc::mojo_client
