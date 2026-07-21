// Copyright 2026 The FydeOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "fydeos-vnc/daemon/pipeline.h"

#include <utility>

#include <base/check.h>
#include <base/files/file_util.h>
#include <base/functional/bind.h>
#include <base/json/json_writer.h>
#include <base/logging.h>
#include <base/strings/string_number_conversions.h>
#include <base/time/time.h>
#include <base/values.h>

#include "fydeos-vnc/daemon/state_paths.h"
#include "fydeos-vnc/net/bind_selector.h"
#include "fydeos-vnc/net/mdns.h"
#include "fydeos-vnc/net/password.h"
#include "fydeos-vnc/net/tls.h"

namespace fydeos::vnc::daemon {

namespace {

constexpr base::TimeDelta kCrtcRetryInterval = base::Milliseconds(500);

}  // namespace

const char* StateToString(Pipeline::State s) {
  switch (s) {
    case Pipeline::State::kDisabled:
      return "Disabled";
    case Pipeline::State::kWaitingForCrtc:
      return "WaitingForCrtc";
    case Pipeline::State::kListening:
      return "Listening";
  }
  return "?";
}

Pipeline::Pipeline(Config config,
                   mojo_client::ChromeClient* chrome,
                   dbus::Bus* bus)
    : config_(std::move(config)), chrome_(chrome), bus_(bus) {
  base::CreateDirectory(config_.state_dir);
  if (!net::EnsureSelfSignedCertificate(CertPath(config_.state_dir),
                                         KeyPath(config_.state_dir))) {
    LOG(ERROR) << "Failed to ensure self-signed cert; daemon will run but "
                  "the cert path will be empty until next start.";
  }
  fingerprint_ =
      net::GetCertificateFingerprint(CertPath(config_.state_dir));
  LOG(INFO) << "TLS cert fingerprint (SHA-256): " << fingerprint_;
}

Pipeline::~Pipeline() {
  Stop();
}

void Pipeline::SetEnabled(bool enabled) {
  if (enabled_ == enabled) {
    return;
  }
  enabled_ = enabled;
  if (enabled_) {
    TryStart();
  } else {
    Stop();
  }
  Notify();
}

Pipeline::Status Pipeline::GetStatus() const {
  Status s;
  s.state = state_;
  s.tls_fingerprint = fingerprint_;
  s.bound_ip = (state_ == State::kListening) ? bound_ip_ : std::string();
  s.bound_port = (state_ == State::kListening) ? config_.port : 0;
  return s;
}

void Pipeline::ReloadCredentials() {
  // Re-read the fingerprint in case the cert was rotated externally.
  fingerprint_ =
      net::GetCertificateFingerprint(CertPath(config_.state_dir));
  if (enabled_ && state_ == State::kListening) {
    Stop();
    TryStart();
  }
  Notify();
}

void Pipeline::ReloadPassword() {
  std::string password_hash;
  auto loaded = net::LoadPasswordHash(PasswordPath(config_.state_dir));
  if (loaded) {
    password_hash = *loaded;
  }
  std::string password_plain;
  auto plain = net::LoadPasswordPlain(PasswordPlainPath(config_.state_dir));
  if (plain) {
    password_plain = *plain;
  }
  if (vnc_server_) {
    vnc_server_->SetPasswordHash(std::move(password_hash));
    vnc_server_->SetPasswordPlain(std::move(password_plain));
  }
  // No Notify(): password set/clear doesn't change pipeline State, only
  // its accepted-credentials set.
}

void Pipeline::SetStatusChangedCallback(StatusChangedCb cb) {
  status_cb_ = std::move(cb);
}

bool Pipeline::SwitchCaptureDisplay(int64_t display_id,
                                    std::string* error_out) {
  DCHECK(error_out);
  if (!selector_) {
    *error_out = "not-running";
    return false;
  }
  switch (selector_->SwitchToDisplay(display_id)) {
    case CaptureSourceSelector::SwitchResult::kOk:
      return true;
    case CaptureSourceSelector::SwitchResult::kNotInFrameSinkMode:
      *error_out = "kms-mode";
      return false;
    case CaptureSourceSelector::SwitchResult::kUnknownDisplay:
      *error_out = "unknown-display";
      return false;
    case CaptureSourceSelector::SwitchResult::kChromeUnavailable:
      *error_out = "chrome-unavailable";
      return false;
  }
  *error_out = "internal-error";
  return false;
}

std::string Pipeline::GetDisplaysJson() const {
  base::DictValue root;
  if (!selector_) {
    root.Set("mode", "disabled");
  } else if (selector_->state() ==
             CaptureSourceSelector::State::kFrameSinkActive) {
    root.Set("mode", "framesink");
  } else {
    root.Set("mode", "kms");
  }
  const int64_t current = selector_ ? selector_->current_display_id() : 0;
  root.Set("current", base::NumberToString(current));
  base::ListValue displays;
  if (selector_) {
    for (const auto& d : selector_->GetDisplays()) {
      base::DictValue entry;
      entry.Set("id", base::NumberToString(d.id));
      entry.Set("name", d.name);
      entry.Set("width", static_cast<int>(d.width));
      entry.Set("height", static_cast<int>(d.height));
      entry.Set("isPrimary", d.is_primary);
      // current == 0 means "unswitched", i.e. the primary display.
      entry.Set("active",
                d.id == current || (current == 0 && d.is_primary));
      displays.Append(std::move(entry));
    }
  }
  root.Set("displays", std::move(displays));
  return base::WriteJson(root).value_or("{}");
}

void Pipeline::TryStart() {
  DCHECK(enabled_);

  capture::CrtcFinder finder;
  finder.SetSpec(config_.display_spec);
  auto probe = finder.Find();
  if (!probe) {
    if (state_ != State::kWaitingForCrtc) {
      LOG(INFO) << "No active CRTC; waiting (panel may be asleep).";
    }
    state_ = State::kWaitingForCrtc;
    if (!retry_timer_.IsRunning()) {
      retry_timer_.Start(FROM_HERE, kCrtcRetryInterval,
                         base::BindRepeating(&Pipeline::OnRetryTick,
                                             base::Unretained(this)));
    }
    return;
  }

  source_width_ = probe->width();
  source_height_ = probe->height();
  probe.reset();

  std::string password_hash;
  auto loaded = net::LoadPasswordHash(PasswordPath(config_.state_dir));
  if (loaded) {
    password_hash = *loaded;
  }
  std::string password_plain;
  auto plain_loaded =
      net::LoadPasswordPlain(PasswordPlainPath(config_.state_dir));
  if (plain_loaded) {
    password_plain = *plain_loaded;
  }

  const uint32_t bind_ipv4 = net::SelectIPv4BindAddress();
  // Format network-order u32 → "a.b.c.d" for status reporting / UI.
  {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&bind_ipv4);
    char buf[16] = {};
    snprintf(buf, sizeof(buf), "%u.%u.%u.%u", p[0], p[1], p[2], p[3]);
    bound_ip_ = buf;
  }
  if (bind_ipv4 == 0) {
    LOG(WARNING) << "No LAN-class interface found; deferring listen "
                    "(never bind to a public wildcard address).";
    state_ = State::kWaitingForCrtc;  // reuse retry slot for now
    if (!retry_timer_.IsRunning()) {
      retry_timer_.Start(FROM_HERE, kCrtcRetryInterval,
                         base::BindRepeating(&Pipeline::OnRetryTick,
                                             base::Unretained(this)));
    }
    return;
  }

  // Selector survives toggle-off (see Stop() comment); only create on
  // first start to avoid the Mesa/EGL teardown deadlock.
  if (!selector_) {
    CaptureSourceSelector::Config sel_cfg;
    sel_cfg.display_spec = config_.display_spec;
    sel_cfg.source_width = source_width_;
    sel_cfg.source_height = source_height_;
    sel_cfg.pre_login_fps = config_.pre_login_fps;
    // Display switch / hotplug / KMS fallback → StatusChanged signal so
    // D-Bus consumers know to re-query ListDisplays.
    sel_cfg.display_state_changed = base::BindRepeating(
        &Pipeline::Notify, base::Unretained(this));
    selector_ = std::make_unique<CaptureSourceSelector>(
        std::move(sel_cfg), chrome_,
        base::BindRepeating(
            [](Pipeline* p, std::unique_ptr<capture::Frame> f) {
              if (p->vnc_server_ && f) {
                p->vnc_server_->PushFrame(*f);
              }
            },
            base::Unretained(this)));
    selector_->Start();
  } else {
    // Selector survived an earlier toggle-off; restart its capture.
    selector_->Resume();
  }

  encoder::VncServer::Options opts;
  opts.source_width = source_width_;
  opts.source_height = source_height_;
  opts.port = config_.port;
  // Dynamic provider — queries Selector each event so we follow the
  // KMS/uinput ↔ FrameSink/Ash transitions automatically. Capturing
  // selector_.get() by raw ptr is fine: VncServer is destroyed before
  // selector_ in Stop().
  opts.input_sink_provider = base::BindRepeating(
      [](CaptureSourceSelector* s) { return s->input_sink(); },
      selector_.get());
  opts.cert_path = CertPath(config_.state_dir).value();
  opts.key_path = KeyPath(config_.state_dir).value();
  opts.password_hash = password_hash;
  opts.password_plain = password_plain;
  opts.listen_ipv4_network_order = bind_ipv4;
  // Capture stops burning CPU when no clients are connected.
  // VncServer fires this when the client count crosses 0 in either
  // direction; we forward to the selector which Pauses/Resumes both
  // KMS and FrameSink sources. selector_ is guaranteed to outlive
  // vnc_server_ (see Stop()), so base::Unretained on a raw pointer
  // captured at TryStart time is safe.
  opts.idle_state_changed = base::BindRepeating(
      [](CaptureSourceSelector* s, bool is_idle) {
        s->SetClientPresence(!is_idle);
        if (is_idle) {
          s->Pause();
        } else {
          s->Resume();
        }
      },
      selector_.get());
  // No clients yet — start the capture pipeline in paused state so we
  // don't burn cycles before the first viewer arrives.
  selector_->SetClientPresence(false);
  selector_->Pause();
  vnc_server_ = std::make_unique<encoder::VncServer>(std::move(opts));
  if (!vnc_server_->Start()) {
    LOG(ERROR) << "Failed to start VNC server";
    vnc_server_.reset();
    // Keep |selector_| alive, same as Stop(): destroying it can deadlock
    // joining the KMS capture thread inside Mesa/EGL. Pause is enough.
    selector_->Pause();
    return;
  }

  // Ask permission_broker to open the port through ChromeOS's firewall.
  // Without this, the default INPUT-policy-DROP chain blocks 5900.
  const std::string iface = net::SelectIPv4BindInterface();
  port_access_ = net::PortAccess::Request(
      bus_, static_cast<uint16_t>(config_.port), iface);
  if (!port_access_) {
    LOG(WARNING) << "permission_broker did not open port " << config_.port
                 << " on " << iface
                 << "; clients may be blocked by the system firewall";
  }
  // The noVNC WebSocket proxy listens on 6080. Daemon holds the
  // permission_broker grant so the firewall rule tracks VNC enable /
  // disable transitions; the actual websockify process is a sibling
  // upstart job (init/fydeos-vnc-wsproxy.conf).
  wsproxy_port_access_ = net::PortAccess::Request(bus_, 6080, iface);
  if (!wsproxy_port_access_) {
    LOG(WARNING)
        << "permission_broker did not open port 6080 on " << iface
        << "; the noVNC web UI will be blocked by the system firewall";
  }

  retry_timer_.Stop();
  state_ = State::kListening;
  mdns_ = net::MdnsPublication::Publish(bus_, config_.port, "fydeos");
  LOG(INFO) << "Pipeline listening on :" << config_.port << " ("
            << source_width_ << "x" << source_height_ << ")";
}

void Pipeline::Stop() {
  retry_timer_.Stop();
  mdns_.reset();
  // VncServer holds a raw pointer to the selector's current InputSink, so
  // tear it down first to avoid use-after-free during shutdown.
  vnc_server_.reset();
  // Deliberately keep |selector_| alive across toggle-off / toggle-on
  // cycles. KmsCaptureSource's worker thread blocks inside Mesa/EGL
  // during glReadPixels; destroying it from the main thread would
  // deadlock std::thread::join() behind a GPU op we can't interrupt.
  // Pause suspends the KMS capture thread (the FrameSink source is
  // damage-driven and already near-zero cost when the screen is
  // static). Resume happens in TryStart.
  if (selector_) {
    // The VncServer (and its clients) are gone; make sure Chrome-side UI
    // driven by client presence goes away too.
    selector_->SetClientPresence(false);
    selector_->Pause();
  }
  // Releasing port_access_ closes our end of the lifeline pipe;
  // permission_broker watches the broker-side end for EOF and removes
  // the iptables rule.
  port_access_.reset();
  wsproxy_port_access_.reset();
  if (enabled_ && state_ != State::kWaitingForCrtc) {
    state_ = State::kWaitingForCrtc;
  } else if (!enabled_) {
    state_ = State::kDisabled;
  }
}

void Pipeline::OnRetryTick() {
  if (!enabled_ || state_ != State::kWaitingForCrtc) {
    retry_timer_.Stop();
    return;
  }
  TryStart();
  if (state_ == State::kListening) {
    Notify();
  }
}

void Pipeline::Notify() {
  if (status_cb_) {
    status_cb_.Run(GetStatus());
  }
}

}  // namespace fydeos::vnc::daemon
