// Copyright 2026 The FydeOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FYDEOS_VNC_ENCODER_VNC_SERVER_H_
#define FYDEOS_VNC_ENCODER_VNC_SERVER_H_

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <base/functional/callback.h>
#include <base/memory/raw_ptr.h>
#include <base/sequence_checker.h>
#include <base/timer/timer.h>
#include <rfb/rfb.h>

#include <list>

#include <openssl/ssl.h>

#include "fydeos-vnc/capture/capture_source.h"
#include "fydeos-vnc/input/input_sink.h"
#include "fydeos-vnc/net/tcp_listener.h"
#include "fydeos-vnc/net/vencrypt_session.h"

namespace fydeos::vnc::encoder {

// libvncserver wrapper. Owns the rfbScreenInfoPtr, the BGRA framebuffer
// (right-padded to a multiple of 4 pixels), the rfbProcessEvents tick
// timer, and the TCP listeners + per-connection VeNCryptSessions that
// terminate TLS before libvncserver sees the byte stream.
// Single-threaded; lives on the daemon's main task runner.
//
// Security surface (see VeNCryptSession for the wire details):
//   - LAN clients: VeNCrypt X509Plain when a password is set (bcrypt
//     verified), X509None otherwise. Legacy VncAuth is additionally
//     advertised only in "allow insecure clients" mode.
//   - Loopback (on-device websockify for noVNC): sec-type None without
//     a password, VeNCrypt Plain with one — the outer wss:// already
//     encrypts that path.
class VncServer {
 public:
  // Returns the input sink to dispatch a given event to. Queried per-event
  // so the underlying sink can change (e.g., CaptureSourceSelector flipping
  // between uinput and the Ash injector) without VncServer caring.
  using InputSinkProvider = base::RepeatingCallback<input::InputSink*()>;

  struct Options {
    // Source frame dimensions (pre-padding).
    uint32_t source_width;
    uint32_t source_height;
    int port;
    InputSinkProvider input_sink_provider;
    // Paths to PEM-encoded cert + key. Both must exist and be readable;
    // EnsureSelfSignedCertificate() should be called first.
    std::string cert_path;
    std::string key_path;
    // Optional bcrypt hash. Empty = no password (X509None only).
    std::string password_hash;
    // Optional plaintext password mirror. Empty = VncAuth NOT advertised.
    // Non-empty implies user opted into "allow insecure clients" mode;
    // VncAuth requires the server to hold the password in plaintext to
    // do DES challenge-response, so this is a deliberate trade-off.
    std::string password_plain;
    // IPv4 listen address in *network* byte order. Zero = INADDR_ANY (which
    // is what we want only if no LAN interface is present yet — see design
    // §5.7.1).
    uint32_t listen_ipv4_network_order = 0;
    // Fired when the client count transitions across 0. |is_idle| is
    // true on 1 → 0, false on 0 → 1. Pipeline uses this to pause /
    // resume the capture pipeline so the daemon doesn't burn CPU on
    // captures nobody is consuming.
    base::RepeatingCallback<void(bool is_idle)> idle_state_changed;
  };

  explicit VncServer(Options opts);
  VncServer(const VncServer&) = delete;
  VncServer& operator=(const VncServer&) = delete;
  ~VncServer();

  // Start listening. Returns false on failure.
  bool Start();
  void Stop();

  // Replace the bcrypt password hash advertised to incoming VeNCrypt
  // sessions. Existing connected clients are unaffected; new accepts
  // pick up the new value. Cheap, non-blocking — does NOT restart the
  // pipeline.
  void SetPasswordHash(std::string hash);

  // Replace the plaintext password mirror used for the legacy VncAuth
  // path. Empty string disables VncAuth advertising. Same non-blocking
  // semantics as SetPasswordHash.
  void SetPasswordPlain(std::string plain);

  // Called from the daemon's main thread with a captured frame. Copies
  // into the libvncserver framebuffer and marks |frame.damage| dirty
  // (whole frame when the damage list is empty, e.g. the KMS source).
  void PushFrame(const capture::Frame& frame);

  // Device → viewer clipboard. Caches |utf8| and announces it to every
  // connected client: Extended Clipboard notify (the client pulls via
  // request → provide) for capable clients, legacy Latin-1
  // ServerCutText for the rest.
  void PushClipboard(std::string utf8);

  // libvncserver event callbacks (public so the static thunks can call
  // them; not meant for external use).
  void OnKeyEvent(bool down, uint32_t keysym);
  void OnPointerEvent(int button_mask, int x, int y);
  void OnClipboard(std::string_view utf8);
  // Latest device clipboard text (set via PushClipboard). Read by the
  // Extended Clipboard thunks to answer client request/peek messages.
  const std::string& latest_clipboard() const { return latest_clipboard_; }
  // libvncserver new-client / client-gone thunks call these to keep
  // client_count_ tracked and to fire idle_state_changed across the
  // 0-clients ↔ ≥1-clients boundary.
  void OnClientConnected();
  void OnClientDisconnected();

 private:
  void ProcessEventsTick();
  // Each accepted TCP connection spins up a VeNCryptSession that runs
  // the handshake on a worker thread and posts back here on success.
  void OnClientAccepted(base::ScopedFD tcp_fd, std::string peer_ip);
  void OnClientAuthenticated(base::ScopedFD plain_fd, std::string peer_ip);
  void OnSessionFinished(net::VeNCryptSession* session);

  // password_hash is mutated live by SetPasswordHash; all other fields
  // are effectively immutable after construction.
  Options opts_;
  // Source dims mutate when PushFrame sees a new size (display rotation,
  // monitor hotplug). vnc_width_ stays = source_width_ rounded to 4.
  uint32_t source_width_;
  uint32_t source_height_;
  uint32_t vnc_width_;
  const int port_;

  std::vector<uint32_t> framebuffer_;  // vnc_width_ * source_height_ BGRA px
  // Device clipboard cache for the device→viewer direction.
  std::string latest_clipboard_;
  rfbScreenInfoPtr server_ = nullptr;
  int client_count_ = 0;
  // Set to true when a new client connects (or the capture pipeline
  // resumes after an idle pause). The next PushFrame marks the entire
  // framebuffer dirty regardless of |frame.damage|, so clients get a
  // fresh full image instead of incrementally patching whatever stale
  // content libvncserver was holding.
  bool force_full_mark_ = true;
  base::RepeatingTimer event_timer_;
  net::TcpListener listener_;
  SSL_CTX* ssl_ctx_ = nullptr;
  // Pending VeNCrypt sessions. Each session removes itself via
  // OnSessionFinished once its worker thread has fully torn down.
  std::list<std::unique_ptr<net::VeNCryptSession>> sessions_;
  bool started_ = false;

  SEQUENCE_CHECKER(sequence_checker_);
};

}  // namespace fydeos::vnc::encoder

#endif  // FYDEOS_VNC_ENCODER_VNC_SERVER_H_
