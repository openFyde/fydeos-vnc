// Copyright 2026 The FydeOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FYDEOS_VNC_DAEMON_PIPELINE_H_
#define FYDEOS_VNC_DAEMON_PIPELINE_H_

#include <memory>
#include <string>

#include <base/files/file_path.h>
#include <base/functional/callback.h>
#include <base/timer/timer.h>

#include "fydeos-vnc/capture/crtc.h"
#include "fydeos-vnc/daemon/capture_source_selector.h"
#include "fydeos-vnc/encoder/vnc_server.h"
#include "fydeos-vnc/net/mdns.h"
#include "fydeos-vnc/net/port_access.h"

namespace dbus {
class Bus;
}

namespace fydeos::vnc::mojo_client {
class ChromeClient;
}

namespace fydeos::vnc::daemon {

// One-shot pipeline owner: capture source + uinput sink + libvncserver.
// Survives toggle-off (capture/server torn down, daemon stays resident per
// design §5.1). Handles CRTC probe retry when DPMS is off / panel asleep.
class Pipeline {
 public:
  enum class State {
    kDisabled,        // toggle off, no listening socket
    kWaitingForCrtc,  // toggle on, polling for an active CRTC
    kListening,       // toggle on, libvncserver up
  };

  struct Config {
    base::FilePath state_dir;
    int port = 5900;
    capture::CrtcFinder::Spec display_spec =
        capture::CrtcFinder::Spec::kAnyDisplay;
    int pre_login_fps = 5;
  };

  struct Status {
    State state;
    std::string peer_ip;          // empty when no client; populated on connect
    std::string tls_fingerprint;  // SHA-256 of currently active cert
    std::string bound_ip;         // dotted-decimal LAN IPv4 we're listening on
    int bound_port;
  };

  using StatusChangedCb =
      base::RepeatingCallback<void(const Status&)>;

  // |chrome| may be null; in that case only KMS+uinput paths are used.
  // |bus| is the system D-Bus; used to talk to permission_broker.
  Pipeline(Config config,
           mojo_client::ChromeClient* chrome,
           dbus::Bus* bus);
  Pipeline(const Pipeline&) = delete;
  Pipeline& operator=(const Pipeline&) = delete;
  ~Pipeline();

  // Toggle. Returns immediately; transitions are asynchronous.
  void SetEnabled(bool enabled);
  bool enabled() const { return enabled_; }

  // Directory holding cert/key/password files (--state-dir).
  const base::FilePath& state_dir() const { return config_.state_dir; }

  Status GetStatus() const;

  // Re-load TLS cert + password hash from disk (after Settings rotated
  // either). If the pipeline is currently listening, it is restarted so the
  // new values take effect.
  void ReloadCredentials();

  // Cheap password-only update: reads /var/lib/fydeos-vncd/password.bcrypt
  // and pushes it into the running VncServer. Does NOT restart the
  // pipeline (avoids a teardown-time capture-thread join that can
  // deadlock the main loop). Use this for SetPassword / ClearPassword;
  // ReloadCredentials still restarts for cert rotation.
  void ReloadPassword();

  // Notified on every state transition. The callback runs on the same
  // sequence as SetEnabled.
  void SetStatusChangedCallback(StatusChangedCb cb);

 private:
  void TryStart();
  void Stop();
  void OnRetryTick();
  void Notify();

  const Config config_;
  mojo_client::ChromeClient* const chrome_;
  dbus::Bus* const bus_;
  bool enabled_ = false;
  State state_ = State::kDisabled;
  std::string fingerprint_;
  std::string bound_ip_;  // populated when state_ == kListening
  uint32_t source_width_ = 0;
  uint32_t source_height_ = 0;

  std::unique_ptr<CaptureSourceSelector> selector_;
  std::unique_ptr<encoder::VncServer> vnc_server_;
  std::unique_ptr<net::PortAccess> port_access_;
  // Companion permission_broker grant for the noVNC WebSocket proxy
  // (port 6080). Held in lock-step with port_access_ for the main RFB
  // port; both are released in Pipeline::Stop().
  std::unique_ptr<net::PortAccess> wsproxy_port_access_;
  std::unique_ptr<net::MdnsPublication> mdns_;

  base::RepeatingTimer retry_timer_;
  StatusChangedCb status_cb_;
};

const char* StateToString(Pipeline::State s);

}  // namespace fydeos::vnc::daemon

#endif  // FYDEOS_VNC_DAEMON_PIPELINE_H_
