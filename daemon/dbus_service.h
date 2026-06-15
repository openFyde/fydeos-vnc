// Copyright 2026 The FydeOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FYDEOS_VNC_DAEMON_DBUS_SERVICE_H_
#define FYDEOS_VNC_DAEMON_DBUS_SERVICE_H_

#include <memory>
#include <string>

#include <base/memory/raw_ptr.h>
#include <brillo/dbus/async_event_sequencer.h>
#include <brillo/dbus/dbus_object.h>
#include <dbus/bus.h>

#include "fydeos-vnc/daemon/pipeline.h"

namespace fydeos::vnc::daemon {

// D-Bus surface for org.fydeos.VncManager (see design §6.2). All handlers
// are synchronous — the Pipeline calls are fast (sub-ms apart from CRTC
// probe and TLS cert IO).
class VncManagerService {
 public:
  VncManagerService(scoped_refptr<dbus::Bus> bus, Pipeline* pipeline);
  VncManagerService(const VncManagerService&) = delete;
  VncManagerService& operator=(const VncManagerService&) = delete;
  ~VncManagerService();

  void RegisterAsync(brillo::dbus_utils::AsyncEventSequencer* sequencer);

 private:
  // Method handlers (signatures match the D-Bus types in §6.2).
  void HandleSetEnabled(bool enabled);
  void HandleGetStatus(std::string* state_out, std::string* peer_ip_out,
                       std::string* fingerprint_out,
                       std::string* bound_ip_out);
  std::string HandleRegenerateCertificate();
  void HandleSetPassword(const std::string& password);
  void HandleClearPassword();
  void HandleSetInsecurePassword(const std::string& password);
  void HandleClearInsecurePassword();

  void OnPipelineStatusChanged(const Pipeline::Status& s);

  using StatusSignal =
      brillo::dbus_utils::DBusSignal<std::string, std::string>;

  base::raw_ptr<Pipeline> pipeline_;
  std::unique_ptr<brillo::dbus_utils::DBusObject> dbus_object_;
  std::weak_ptr<StatusSignal> status_changed_signal_;
};

}  // namespace fydeos::vnc::daemon

#endif  // FYDEOS_VNC_DAEMON_DBUS_SERVICE_H_
