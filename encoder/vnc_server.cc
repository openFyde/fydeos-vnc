// Copyright 2026 The FydeOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "fydeos-vnc/encoder/vnc_server.h"

#include <cstring>
#include <vector>

#include <base/check.h>
#include <base/functional/bind.h>
#include <base/logging.h>
#include <base/time/time.h>

#include "fydeos-vnc/encoder/extended_clipboard.h"
#include "fydeos-vnc/encoder/vnc_utils.h"
#include "fydeos-vnc/net/tls.h"

namespace fydeos::vnc::encoder {

namespace {

constexpr int kBitsPerSample = 8;
constexpr int kSamplesPerPixel = 3;
constexpr int kBytesPerPixel = 4;

// rfbProcessEvents tick. 16ms == ~60 Hz; matches upstream kmsvnc's
// `deferUpdateTime`. Independent of capture rate.
constexpr base::TimeDelta kEventTick = base::Milliseconds(16);

VncServer* AsServer(rfbClientPtr cl) {
  return static_cast<VncServer*>(cl->screen->screenData);
}

void KbdAddEventThunk(rfbBool down, rfbKeySym key, rfbClientPtr cl) {
  AsServer(cl)->OnKeyEvent(down != 0, static_cast<uint32_t>(key));
}

void PtrAddEventThunk(int button_mask, int x, int y, rfbClientPtr cl) {
  AsServer(cl)->OnPointerEvent(button_mask, x, y);
  rfbDefaultPtrAddEvent(button_mask, x, y, cl);
}

// Sends an Extended Clipboard message: the 8-byte ServerCutText header
// carries a signed-negative length to mark the extended format, then
// the binary body follows.
void SendExtClipMessage(rfbClientPtr cl, const std::string& body) {
  const int32_t wire_len = -static_cast<int32_t>(body.size());
  uint8_t header[8] = {};
  header[0] = 3;  // rfbServerCutText
  header[4] = static_cast<uint8_t>((wire_len >> 24) & 0xFF);
  header[5] = static_cast<uint8_t>((wire_len >> 16) & 0xFF);
  header[6] = static_cast<uint8_t>((wire_len >> 8) & 0xFF);
  header[7] = static_cast<uint8_t>(wire_len & 0xFF);
  rfbWriteExact(cl, reinterpret_cast<const char*>(header), sizeof(header));
  rfbWriteExact(cl, body.data(), static_cast<int>(body.size()));
}

extern rfbProtocolExtension kExtClipExtension;  // defined below

// Per-client Extended Clipboard state is the raw flags word of the
// client's caps message (0 until received; the action bits state which
// messages the client accepts from us). It is stored INLINE in the
// libvncserver extension data-pointer slot — deliberately not heap
// allocated: libvncserver has no reliable per-client free point (the
// close hook runs at rfbCloseClient, but the client can still be
// iterated until it is reaped), so a pointer would dangle.
//
// Returns the slot if the client enabled the extension, else nullptr.
// Slot presence == "client speaks Extended Clipboard".
void** ExtClipDataSlot(rfbClientPtr cl) {
  for (rfbExtensionData* d = cl->extensions; d; d = d->next) {
    if (d->extension == &kExtClipExtension) {
      return &d->data;
    }
  }
  return nullptr;
}

uint32_t ExtClipClientCaps(void** slot) {
  return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(*slot));
}

// Sends the legacy ServerCutText message. The wire format is Latin-1
// per the RFB spec, so transliterate: code points ≤ 0xFF pass through,
// anything else becomes '?'.
void SendLegacyCutText(rfbClientPtr cl, const std::string& utf8) {
  std::string latin1;
  latin1.reserve(utf8.size());
  for (size_t i = 0; i < utf8.size();) {
    const uint8_t c = static_cast<uint8_t>(utf8[i]);
    uint32_t cp = 0;
    size_t n = 0;
    if (c < 0x80) {
      cp = c;
      n = 1;
    } else if ((c & 0xE0) == 0xC0) {
      cp = c & 0x1F;
      n = 2;
    } else if ((c & 0xF0) == 0xE0) {
      cp = c & 0x0F;
      n = 3;
    } else if ((c & 0xF8) == 0xF0) {
      cp = c & 0x07;
      n = 4;
    } else {
      ++i;  // stray continuation byte; skip
      continue;
    }
    if (i + n > utf8.size()) {
      break;  // truncated sequence at end of string
    }
    bool valid = true;
    for (size_t k = 1; k < n; ++k) {
      const uint8_t cc = static_cast<uint8_t>(utf8[i + k]);
      if ((cc & 0xC0) != 0x80) {
        valid = false;
        break;
      }
      cp = (cp << 6) | (cc & 0x3F);
    }
    i += valid ? n : 1;
    if (valid) {
      latin1.push_back(cp <= 0xFF ? static_cast<char>(cp) : '?');
    }
  }
  uint8_t header[8] = {};
  header[0] = 3;  // rfbServerCutText
  const uint32_t len = static_cast<uint32_t>(latin1.size());
  header[4] = static_cast<uint8_t>((len >> 24) & 0xFF);
  header[5] = static_cast<uint8_t>((len >> 16) & 0xFF);
  header[6] = static_cast<uint8_t>((len >> 8) & 0xFF);
  header[7] = static_cast<uint8_t>(len & 0xFF);
  rfbWriteExact(cl, reinterpret_cast<const char*>(header), sizeof(header));
  if (!latin1.empty()) {
    rfbWriteExact(cl, latin1.data(), static_cast<int>(latin1.size()));
  }
}

// RFB ClientCutText callback. With the FydeOS libvncserver patch, |len|
// is the *signed* length on the wire:
//   * len > 0  → legacy Latin-1 cut text (msg.cct.length raw bytes)
//   * len < 0  → Extended Clipboard payload (TigerVNC-compatible);
//                |len| bytes of binary protocol containing flags + a
//                zlib-compressed UTF-8 stream for the TEXT format
//   * len == 0 → empty cut text; nothing to do.
void SetXCutTextThunk(char* str, int len, rfbClientPtr cl) {
  if (!str || len == 0) {
    return;
  }
  if (len > 0) {
    AsServer(cl)->OnClipboard(std::string_view(str, static_cast<size_t>(len)));
    return;
  }
  // Extended Clipboard payload. We must dispatch the inbound message
  // and, on a Notify-TEXT, send a Request back to actually pull the
  // text — TigerVNC won't send Provide unsolicited.
  const size_t body_len = static_cast<size_t>(-static_cast<int64_t>(len));
  auto msg = extclip::Parse(str, body_len);
  switch (msg.kind) {
    case extclip::InboundKind::kCaps:
      // Remember which actions the client accepts so the device→viewer
      // push (PushClipboard) can pick notify vs direct provide.
      if (void** slot = ExtClipDataSlot(cl)) {
        *slot = reinterpret_cast<void*>(
            static_cast<uintptr_t>(msg.caps_flags));
      }
      break;
    case extclip::InboundKind::kNotifyText:
      SendExtClipMessage(cl, extclip::BuildRequestBody());
      LOG(INFO) << "ExtClip: requested TEXT from client";
      break;
    case extclip::InboundKind::kProvideText:
      AsServer(cl)->OnClipboard(msg.text);
      break;
    case extclip::InboundKind::kRequestText:
      // Client pulls the text we announced via notify (device→viewer).
      SendExtClipMessage(
          cl, extclip::BuildProvideTextBody(AsServer(cl)->latest_clipboard()));
      LOG(INFO) << "ExtClip: provided TEXT to client";
      break;
    case extclip::InboundKind::kPeek:
      SendExtClipMessage(
          cl, extclip::BuildNotifyBody(
                  AsServer(cl)->latest_clipboard().empty()
                      ? 0
                      : extclip::kFmtText));
      break;
    case extclip::InboundKind::kIgnored:
      break;
  }
}

// libvncserver fires this when the connection closes (TCP EOF, error,
// or rfbCloseClient). We use it to maintain client_count_ and trigger
// the idle / active transition in the capture pipeline.
void ClientGoneHook(rfbClientPtr cl) {
  AsServer(cl)->OnClientDisconnected();
}

// Called by libvncserver when a new RFB client connects. Standard
// encoding negotiation runs unmodified (Tight by default for modern
// clients — roughly 5× lower bandwidth than Raw).
rfbNewClientAction NewClientHook(rfbClientPtr cl) {
  LOG(INFO) << "RFB client connected: "
            << (cl->host ? cl->host : "<unknown>");
  cl->clientGoneHook = ClientGoneHook;
  AsServer(cl)->OnClientConnected();
  return RFB_CLIENT_ACCEPT;
}

// Extension hook: returns TRUE iff |encodingNumber| is one of our
// supported pseudo-encodings. Called by libvncserver as the client
// sends its SetEncodings list; the first TRUE return enables the
// extension on that client, which is our cue to send the server's
// Extended Clipboard caps. encodingNumber==0 is the spec's reset
// signal — nothing to do.
int kExtClipPseudoEncodings[] = {extclip::kPseudoEncoding, 0};

rfbBool ExtClipEnablePseudoEncoding(rfbClientPtr cl,
                                    void** /*data*/,
                                    int encodingNumber) {
  if (encodingNumber != extclip::kPseudoEncoding) {
    return FALSE;
  }
  // Runs on the first SetEncodings carrying our pseudo-encoding (and
  // again on any re-negotiation, which is harmless: re-sending caps is
  // spec-legal and the notify at most makes the client re-pull).
  SendExtClipMessage(cl, extclip::BuildCapsBody());
  LOG(INFO) << "ExtClip: advertised TEXT caps to client";
  // Sync the device clipboard to a freshly connected viewer: announce
  // cached text; the client pulls it with request → provide.
  if (!AsServer(cl)->latest_clipboard().empty()) {
    SendExtClipMessage(cl, extclip::BuildNotifyBody());
  }
  return TRUE;
}

rfbProtocolExtension kExtClipExtension = {
    .newClient = nullptr,
    .init = nullptr,
    .pseudoEncodings = kExtClipPseudoEncodings,
    .enablePseudoEncoding = ExtClipEnablePseudoEncoding,
    .handleMessage = nullptr,
    .close = nullptr,
    .usage = nullptr,
    .processArgument = nullptr,
    .next = nullptr,
};

}  // namespace

VncServer::VncServer(Options opts)
    : opts_(std::move(opts)),
      source_width_(opts_.source_width),
      source_height_(opts_.source_height),
      vnc_width_(GetVncWidth(opts_.source_width)),
      port_(opts_.port),
      framebuffer_(vnc_width_ * source_height_, 0) {
  CHECK(opts_.input_sink_provider);
}

VncServer::~VncServer() {
  Stop();
}

bool VncServer::Start() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  CHECK(!started_);

  // Cert/key were ensured on disk by the daemon's startup path; refuse
  // to listen without them — VeNCrypt X.509 is the primary auth path.
  if (opts_.cert_path.empty() || opts_.key_path.empty()) {
    LOG(ERROR) << "VeNCrypt requires a cert/key pair; refusing to start";
    return false;
  }
  ssl_ctx_ = net::MakeServerSSLContext(base::FilePath(opts_.cert_path),
                                       base::FilePath(opts_.key_path));
  if (!ssl_ctx_) {
    LOG(ERROR) << "MakeServerSSLContext failed";
    return false;
  }

  server_ = rfbGetScreen(/*argc=*/nullptr, /*argv=*/nullptr,
                          static_cast<int>(vnc_width_),
                          static_cast<int>(source_height_),
                          kBitsPerSample, kSamplesPerPixel, kBytesPerPixel);
  if (!server_) {
    LOG(ERROR) << "rfbGetScreen failed";
    return false;
  }

  // BGRA8888 little-endian: R=byte2, G=byte1, B=byte0.
  server_->serverFormat.redMax = 255;
  server_->serverFormat.greenMax = 255;
  server_->serverFormat.blueMax = 255;
  server_->serverFormat.redShift = 16;
  server_->serverFormat.greenShift = 8;
  server_->serverFormat.blueShift = 0;

  server_->frameBuffer = reinterpret_cast<char*>(framebuffer_.data());
  // The daemon owns the listen sockets directly so VeNCrypt + TLS
  // termination can happen before libvncserver sees the stream. Setting
  // port = -1 tells rfbInitServer to skip rfbListenOnTCPPort entirely.
  server_->port = -1;
  server_->ipv6port = -1;
  server_->screenData = this;
  server_->kbdAddEvent = KbdAddEventThunk;
  server_->ptrAddEvent = PtrAddEventThunk;
  server_->setXCutText = SetXCutTextThunk;

  // Register the Extended Clipboard pseudo-encoding handler (process-
  // global; libvncserver dispatches to its enablePseudoEncoding hook
  // when any client advertises -1063 in its SetEncodings list).
  static bool extension_registered = false;
  if (!extension_registered) {
    rfbRegisterProtocolExtension(&kExtClipExtension);
    extension_registered = true;
  }
  server_->newClientHook = NewClientHook;
  server_->handleEventsEagerly = TRUE;

  // Ship a classic black arrow cursor to the client. The captured
  // framebuffer does NOT include the OS pointer (Iris GPU uses a HW
  // cursor overlay that bypasses Ash's composited layer), so without
  // server-supplied cursor data the client sees only its local
  // crosshair. This 12×19 arrow gives a reasonable pointer at the
  // right position. xhot/yhot = (0,0) so the tip aligns with the
  // cursor coordinate, matching how Ash reports it.
  {
    static const char kArrowShape[] =
        "x           "
        "xx          "
        "x x         "
        "x  x        "
        "x   x       "
        "x    x      "
        "x     x     "
        "x      x    "
        "x       x   "
        "x        x  "
        "x         x "
        "x    xxxxxxx"
        "x   x x     "
        "x  x   x    "
        "x x    x    "
        "xx      x   "
        "x       x   "
        "         x  "
        "         x  ";
    rfbCursorPtr arrow = rfbMakeXCursor(/*width=*/12, /*height=*/19,
                                        const_cast<char*>(kArrowShape),
                                        /*maskString=*/nullptr);
    arrow->xhot = 0;
    arrow->yhot = 0;
    rfbSetCursor(server_, arrow);
  }

  rfbInitServer(server_);
  if (!rfbIsActive(server_)) {
    LOG(ERROR) << "rfbInitServer failed; is port " << port_ << " in use?";
    rfbScreenCleanup(server_);
    server_ = nullptr;
    return false;
  }

  event_timer_.Start(FROM_HERE, kEventTick,
                     base::BindRepeating(&VncServer::ProcessEventsTick,
                                         base::Unretained(this)));

  // Bring up our own TCP listen sockets and hand accepted FDs to
  // libvncserver via rfbNewClient. libvncserver itself does not listen
  // (server_->port == -1 above).
  if (!listener_.Start(
          opts_.listen_ipv4_network_order, port_,
          base::BindRepeating(&VncServer::OnClientAccepted,
                              base::Unretained(this)))) {
    LOG(ERROR) << "TcpListener::Start failed on port " << port_;
    event_timer_.Stop();
    rfbScreenCleanup(server_);
    server_ = nullptr;
    return false;
  }

  started_ = true;
  LOG(INFO) << "VNC server listening on port " << port_
            << " (frame " << vnc_width_ << "x" << source_height_
            << "; TLS=mandatory, password="
            << (opts_.password_hash.empty() ? "no" : "yes") << ")";
  return true;
}

void VncServer::Stop() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  listener_.Stop();
  // Tear down any in-flight VeNCrypt sessions before destroying the
  // libvncserver state; each session's destructor joins its worker
  // thread so this is the safe ordering.
  sessions_.clear();
  event_timer_.Stop();
  if (server_) {
    rfbShutdownServer(server_, /*disconnectClients=*/TRUE);
    rfbScreenCleanup(server_);
    server_ = nullptr;
  }
  if (ssl_ctx_) {
    SSL_CTX_free(ssl_ctx_);
    ssl_ctx_ = nullptr;
  }
  started_ = false;
}

void VncServer::OnClientAccepted(base::ScopedFD tcp_fd, std::string peer_ip) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!server_ || !ssl_ctx_) {
    return;
  }
  // The VeNCryptSession invokes |on_finished_(this)| when its worker
  // thread fully exits, so we can locate + delete it without needing
  // to thread an iterator through callbacks.
  auto session = std::make_unique<net::VeNCryptSession>(
      std::move(tcp_fd), peer_ip, ssl_ctx_, opts_.password_hash,
      opts_.password_plain,
      base::BindOnce(&VncServer::OnClientAuthenticated,
                     base::Unretained(this)),
      base::BindOnce(&VncServer::OnSessionFinished,
                     base::Unretained(this)));
  session->Start();
  sessions_.push_back(std::move(session));
}

void VncServer::OnClientAuthenticated(base::ScopedFD plain_fd,
                                       std::string peer_ip) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!server_) {
    return;
  }
  // Hand the post-TLS plain end to libvncserver. Switch the client
  // state to INITIALISATION right after creation so libvncserver
  // skips its own version+security exchange (those already ran in
  // VeNCryptSession on the TLS side).
  rfbClientPtr cl = rfbNewClient(server_, plain_fd.release());
  if (!cl) {
    LOG(WARNING) << "rfbNewClient failed for peer " << peer_ip;
    return;
  }
  // Skip libvncserver's own protocol-version + security exchange; the
  // VeNCrypt session already handled those on the TLS-wrapped TCP side.
  // The first byte coming through the socketpair will be the client's
  // ClientInit shared-flag, which is exactly what RFB_INITIALISATION
  // expects. The enum lives anonymously inside _rfbClientRec, so it
  // isn't accessible by bare name in C++ — assign by integer value
  // (index 3 in the state enum, see rfb/rfb.h).
  cl->state = static_cast<decltype(cl->state)>(3);  // RFB_INITIALISATION
  if (cl->host) {
    free(cl->host);
  }
  cl->host = strdup(peer_ip.c_str());
}

void VncServer::OnSessionFinished(net::VeNCryptSession* session) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  // Find and erase. N is small (active VNC viewers per device), so
  // linear scan is fine.
  for (auto it = sessions_.begin(); it != sessions_.end(); ++it) {
    if (it->get() == session) {
      sessions_.erase(it);
      return;
    }
  }
}

void VncServer::SetPasswordHash(std::string hash) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  opts_.password_hash = std::move(hash);
  LOG(INFO) << "VeNCrypt: password_hash updated live (length="
            << opts_.password_hash.size() << ")";
}

void VncServer::SetPasswordPlain(std::string plain) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  const bool was_enabled = !opts_.password_plain.empty();
  opts_.password_plain = std::move(plain);
  const bool now_enabled = !opts_.password_plain.empty();
  if (was_enabled != now_enabled) {
    LOG(INFO) << "VncAuth advertise mode "
              << (now_enabled ? "ENABLED" : "disabled")
              << " (insecure clients)";
  }
}

void VncServer::PushFrame(const capture::Frame& frame) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!started_) {
    return;
  }
  // Resize libvncserver's framebuffer if the source dimensions changed
  // (display rotation, monitor hotplug, viz sending logical-pixel size
  // rather than the KMS native size, etc.). Cheap when dims match.
  if (frame.width != source_width_ || frame.height != source_height_) {
    LOG(INFO) << "Frame dims changed: server " << source_width_ << "x"
              << source_height_ << " → " << frame.width << "x"
              << frame.height << "; resizing framebuffer";
    source_width_ = frame.width;
    source_height_ = frame.height;
    vnc_width_ = GetVncWidth(source_width_);
    framebuffer_.assign(static_cast<size_t>(vnc_width_) * source_height_, 0);
    // rfbNewFramebuffer wires the new buffer + dims into libvncserver and
    // tells all connected clients to expect the new size. It also resets
    // serverFormat to libvncserver defaults (RGBA), so we must re-apply
    // our BGRA channel shifts after.
    rfbNewFramebuffer(server_, reinterpret_cast<char*>(framebuffer_.data()),
                      static_cast<int>(vnc_width_),
                      static_cast<int>(source_height_),
                      kBitsPerSample, kSamplesPerPixel, kBytesPerPixel);
    server_->serverFormat.redMax = 255;
    server_->serverFormat.greenMax = 255;
    server_->serverFormat.blueMax = 255;
    server_->serverFormat.redShift = 16;
    server_->serverFormat.greenShift = 8;
    server_->serverFormat.blueShift = 0;
  }
  ConvertBuffer(frame, framebuffer_.data(), vnc_width_);
  // Prefer damage-driven encoding. viz hands us a per-frame list of
  // dirty rectangles; libvncserver re-encodes (and re-sends) only the
  // marked area for each client. A 1080p frame with a moving cursor
  // sprite typically reports a single ~32×32 rect, which collapses
  // encoding work from 8 MB / frame to a few kB.
  //
  // KMS / EGL source can't report damage; |frame.damage| is empty for
  // that path and we fall back to whole-frame marking.
  if (frame.damage.empty() || force_full_mark_) {
    force_full_mark_ = false;
    rfbMarkRectAsModified(server_, 0, 0, static_cast<int>(vnc_width_),
                          static_cast<int>(source_height_));
  } else {
    const int max_x = static_cast<int>(vnc_width_);
    const int max_y = static_cast<int>(source_height_);
    for (const auto& r : frame.damage) {
      int x1 = static_cast<int>(r.x);
      int y1 = static_cast<int>(r.y);
      int x2 = x1 + static_cast<int>(r.w);
      int y2 = y1 + static_cast<int>(r.h);
      if (x1 < 0) x1 = 0;
      if (y1 < 0) y1 = 0;
      if (x2 > max_x) x2 = max_x;
      if (y2 > max_y) y2 = max_y;
      if (x2 > x1 && y2 > y1) {
        rfbMarkRectAsModified(server_, x1, y1, x2, y2);
      }
    }
  }
}

void VncServer::OnKeyEvent(bool down, uint32_t keysym) {
  if (auto* sink = opts_.input_sink_provider.Run()) {
    sink->InjectKey(keysym, down);
  }
}

void VncServer::OnPointerEvent(int button_mask, int x, int y) {
  if (auto* sink = opts_.input_sink_provider.Run()) {
    sink->InjectPointer(x, y, static_cast<uint8_t>(button_mask));
  }
}

void VncServer::OnClipboard(std::string_view utf8) {
  if (auto* sink = opts_.input_sink_provider.Run()) {
    sink->InjectClipboard(utf8);
  }
}

void VncServer::PushClipboard(std::string utf8) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  latest_clipboard_ = std::move(utf8);
  if (!started_ || !server_) {
    return;
  }
  int pushed = 0;
  rfbClientIteratorPtr it = rfbGetClientIterator(server_);
  while (rfbClientPtr cl = rfbClientIteratorNext(it)) {
    // Only clients past the handshake; the state enum is anonymous
    // inside _rfbClientRec, so compare by index (4 = RFB_NORMAL).
    if (cl->state != static_cast<decltype(cl->state)>(4)) {
      continue;
    }
    if (void** slot = ExtClipDataSlot(cl)) {
      // Extended Clipboard client. Spec flow: notify → client requests
      // → we provide (handled in SetXCutTextThunk). If the client's
      // caps accept provide but not notify, push the text directly.
      const uint32_t caps = ExtClipClientCaps(slot);
      if (caps != 0 && !(caps & extclip::kActNotify) &&
          (caps & extclip::kActProvide)) {
        SendExtClipMessage(cl,
                           extclip::BuildProvideTextBody(latest_clipboard_));
      } else {
        SendExtClipMessage(
            cl, extclip::BuildNotifyBody(
                    latest_clipboard_.empty() ? 0 : extclip::kFmtText));
      }
    } else {
      SendLegacyCutText(cl, latest_clipboard_);
    }
    ++pushed;
  }
  rfbReleaseClientIterator(it);
  if (pushed > 0) {
    VLOG(1) << "Clipboard pushed to " << pushed << " client(s), "
            << latest_clipboard_.size() << " bytes";
  }
}

void VncServer::OnClientConnected() {
  ++client_count_;
  // Stale framebuffer + small damage = black-screen-with-cursor for
  // the new viewer. Force a full re-mark on the next push.
  force_full_mark_ = true;
  if (client_count_ == 1 && opts_.idle_state_changed) {
    opts_.idle_state_changed.Run(/*is_idle=*/false);
  }
}

void VncServer::OnClientDisconnected() {
  if (client_count_ > 0) {
    --client_count_;
  }
  if (client_count_ == 0 && opts_.idle_state_changed) {
    opts_.idle_state_changed.Run(/*is_idle=*/true);
  }
}

void VncServer::ProcessEventsTick() {
  if (!server_ || !rfbIsActive(server_)) {
    return;
  }
  rfbProcessEvents(server_, 0);
}

}  // namespace fydeos::vnc::encoder
