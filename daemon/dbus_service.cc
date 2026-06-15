// Copyright 2026 The FydeOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "fydeos-vnc/daemon/dbus_service.h"

#include <utility>

#include <base/files/file_path.h>
#include <base/files/file_util.h>
#include <base/functional/bind.h>
#include <base/logging.h>
#include <brillo/dbus/dbus_method_response.h>
#include <dbus/object_path.h>

#include "fydeos-vnc/net/password.h"
#include "fydeos-vnc/net/tls.h"

namespace fydeos::vnc::daemon {

namespace {

constexpr char kObjectPath[] = "/org/fydeos/VncManager";
constexpr char kInterface[] = "org.fydeos.VncManager";

}  // namespace

VncManagerService::VncManagerService(scoped_refptr<dbus::Bus> bus,
                                      Pipeline* pipeline)
    : pipeline_(pipeline) {
  CHECK(pipeline_);
  dbus_object_ = std::make_unique<brillo::dbus_utils::DBusObject>(
      /*object_manager=*/nullptr, bus, dbus::ObjectPath(kObjectPath));
}

VncManagerService::~VncManagerService() = default;

void VncManagerService::RegisterAsync(
    brillo::dbus_utils::AsyncEventSequencer* sequencer) {
  auto* iface = dbus_object_->AddOrGetInterface(kInterface);
  iface->AddSimpleMethodHandler("SetEnabled", base::Unretained(this),
                                 &VncManagerService::HandleSetEnabled);
  iface->AddSimpleMethodHandler("GetStatus", base::Unretained(this),
                                 &VncManagerService::HandleGetStatus);
  iface->AddSimpleMethodHandler(
      "RegenerateCertificate", base::Unretained(this),
      &VncManagerService::HandleRegenerateCertificate);
  iface->AddSimpleMethodHandler("SetPassword", base::Unretained(this),
                                 &VncManagerService::HandleSetPassword);
  iface->AddSimpleMethodHandler("ClearPassword", base::Unretained(this),
                                 &VncManagerService::HandleClearPassword);
  iface->AddSimpleMethodHandler(
      "SetInsecurePassword", base::Unretained(this),
      &VncManagerService::HandleSetInsecurePassword);
  iface->AddSimpleMethodHandler(
      "ClearInsecurePassword", base::Unretained(this),
      &VncManagerService::HandleClearInsecurePassword);
  status_changed_signal_ =
      iface->RegisterSignalOfType<StatusSignal>("StatusChanged");

  pipeline_->SetStatusChangedCallback(base::BindRepeating(
      &VncManagerService::OnPipelineStatusChanged, base::Unretained(this)));

  dbus_object_->RegisterAsync(
      sequencer->GetHandler("Failed to export org.fydeos.VncManager", true));
}

void VncManagerService::HandleSetEnabled(bool enabled) {
  LOG(INFO) << "D-Bus SetEnabled(" << enabled << ")";
  pipeline_->SetEnabled(enabled);
}

void VncManagerService::HandleGetStatus(std::string* state_out,
                                         std::string* peer_ip_out,
                                         std::string* fingerprint_out,
                                         std::string* bound_ip_out) {
  const auto s = pipeline_->GetStatus();
  *state_out = StateToString(s.state);
  *peer_ip_out = s.peer_ip;
  *fingerprint_out = s.tls_fingerprint;
  *bound_ip_out = s.bound_ip;
}

std::string VncManagerService::HandleRegenerateCertificate() {
  LOG(INFO) << "D-Bus RegenerateCertificate";
  // Wipe & regenerate. The Pipeline owns the cert paths; for now keep them
  // hardcoded to /var/lib/fydeos-vncd/.
  const base::FilePath dir("/var/lib/fydeos-vncd");
  base::DeleteFile(dir.Append("cert.pem"));
  base::DeleteFile(dir.Append("key.pem"));
  if (!net::EnsureSelfSignedCertificate(dir.Append("cert.pem"),
                                         dir.Append("key.pem"))) {
    LOG(ERROR) << "RegenerateCertificate failed";
    return std::string();
  }
  pipeline_->ReloadCredentials();
  return net::GetCertificateFingerprint(dir.Append("cert.pem"));
}

void VncManagerService::HandleSetPassword(const std::string& password) {
  LOG(INFO) << "D-Bus SetPassword (length=" << password.size() << ")";
  auto hash = net::HashPassword(password);
  if (!hash) {
    LOG(ERROR) << "SetPassword: hash failed";
    return;
  }
  net::SavePasswordHash(base::FilePath("/var/lib/fydeos-vncd/password.bcrypt"),
                        *hash);
  // Password change is non-destructive: just push the new hash into
  // the live VncServer. Restarting the whole pipeline can dead-lock
  // the capture-thread join.
  pipeline_->ReloadPassword();
}

void VncManagerService::HandleClearPassword() {
  LOG(INFO) << "D-Bus ClearPassword";
  base::DeleteFile(
      base::FilePath("/var/lib/fydeos-vncd/password.bcrypt"));
  base::DeleteFile(
      base::FilePath("/var/lib/fydeos-vncd/password.plain"));
  pipeline_->ReloadPassword();
}

// "Insecure" path: caller acknowledges that VncAuth + plaintext RFB are
// being enabled. We mirror the bcrypt-stored password to a plaintext
// file alongside it; both files are written atomically with mode 0600
// (and the directory is owned by the fydeos-vnc system user under
// minijail).
void VncManagerService::HandleSetInsecurePassword(
    const std::string& password) {
  LOG(WARNING) << "D-Bus SetInsecurePassword (length=" << password.size()
               << ") — VncAuth fallback now enabled; RFB traffic from "
                  "VncAuth clients is unencrypted on the LAN.";
  auto hash = net::HashPassword(password);
  if (!hash) {
    LOG(ERROR) << "SetInsecurePassword: hash failed";
    return;
  }
  net::SavePasswordHash(base::FilePath("/var/lib/fydeos-vncd/password.bcrypt"),
                        *hash);
  net::SavePasswordPlain(
      base::FilePath("/var/lib/fydeos-vncd/password.plain"), password);
  pipeline_->ReloadPassword();
}

void VncManagerService::HandleClearInsecurePassword() {
  LOG(INFO) << "D-Bus ClearInsecurePassword (VncAuth disabled)";
  base::DeleteFile(
      base::FilePath("/var/lib/fydeos-vncd/password.plain"));
  pipeline_->ReloadPassword();
}

void VncManagerService::OnPipelineStatusChanged(const Pipeline::Status& s) {
  if (auto sig = status_changed_signal_.lock()) {
    sig->Send(StateToString(s.state), s.peer_ip);
  }
}

}  // namespace fydeos::vnc::daemon
