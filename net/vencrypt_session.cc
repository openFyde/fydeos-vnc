// Copyright 2026 The FydeOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "fydeos-vnc/net/vencrypt_session.h"

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <utility>

#include <base/functional/bind.h>
#include <base/logging.h>
#include <base/task/single_thread_task_runner.h>
#include <openssl/err.h>

#include <openssl/rand.h>

#include "fydeos-vnc/net/password.h"

namespace fydeos::vnc::net {

namespace {

// A client that connects and then goes silent would otherwise pin this
// session's worker thread (and fd) forever — blocking reads have no
// deadline. Applied for the handshake only; the relay phase is
// poll-driven and tolerates arbitrarily long idle periods.
constexpr time_t kHandshakeTimeoutSeconds = 30;

void SetSocketTimeouts(int fd, time_t seconds) {
  timeval tv{};
  tv.tv_sec = seconds;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

// Wire constants.
constexpr char kRfbProtoVersion[] = "RFB 003.008\n";  // 12 bytes
constexpr size_t kRfbVersionLen = 12;

constexpr uint8_t kSecTypeNone = 1;
constexpr uint8_t kSecTypeVncAuth = 2;
constexpr uint8_t kSecTypeVeNCrypt = 19;

constexpr uint8_t kVeNCryptMajor = 0;
constexpr uint8_t kVeNCryptMinor = 2;

constexpr uint32_t kSubTypePlain = 256;
constexpr uint32_t kSubTypeX509None = 260;
constexpr uint32_t kSubTypeX509Plain = 262;

// Read/write helpers honoring EINTR/partial.
bool ReadAllFromFd(int fd, void* buf, size_t n) {
  uint8_t* p = static_cast<uint8_t*>(buf);
  while (n > 0) {
    ssize_t r = read(fd, p, n);
    if (r > 0) {
      p += r;
      n -= static_cast<size_t>(r);
      continue;
    }
    if (r == 0) {
      return false;  // peer closed
    }
    if (errno == EINTR) {
      continue;
    }
    PLOG(WARNING) << "ReadAllFromFd";
    return false;
  }
  return true;
}

bool WriteAllToFd(int fd, const void* buf, size_t n) {
  const uint8_t* p = static_cast<const uint8_t*>(buf);
  while (n > 0) {
    ssize_t w = write(fd, p, n);
    if (w > 0) {
      p += w;
      n -= static_cast<size_t>(w);
      continue;
    }
    if (errno == EINTR) {
      continue;
    }
    PLOG(WARNING) << "WriteAllToFd";
    return false;
  }
  return true;
}

void LogSslErr(const char* op) {
  unsigned long e = ERR_get_error();
  char buf[256] = {};
  ERR_error_string_n(e, buf, sizeof(buf));
  LOG(WARNING) << "VeNCrypt: " << op << ": " << buf;
}

}  // namespace

VeNCryptSession::VeNCryptSession(base::ScopedFD tcp_fd,
                                  std::string peer_ip,
                                  SSL_CTX* ssl_ctx,
                                  std::string password_hash,
                                  std::string password_plain,
                                  AuthSuccessCb on_authenticated,
                                  FinishedCb on_finished)
    : tcp_fd_(std::move(tcp_fd)),
      peer_ip_(std::move(peer_ip)),
      ssl_ctx_(ssl_ctx),
      password_hash_(std::move(password_hash)),
      password_plain_(std::move(password_plain)),
      main_runner_(base::SingleThreadTaskRunner::GetCurrentDefault()),
      on_authenticated_(std::move(on_authenticated)),
      on_finished_(std::move(on_finished)) {}

VeNCryptSession::~VeNCryptSession() {
  stop_flag_.store(true);
  // Force socket shutdown so blocking reads in the worker unblock.
  if (tcp_fd_.is_valid()) {
    ::shutdown(tcp_fd_.get(), SHUT_RDWR);
  }
  if (plain_local_fd_.is_valid()) {
    ::shutdown(plain_local_fd_.get(), SHUT_RDWR);
  }
  if (worker_.joinable()) {
    worker_.join();
  }
  if (ssl_) {
    SSL_free(ssl_);
    ssl_ = nullptr;
  }
}

void VeNCryptSession::Start() {
  worker_ = std::thread(&VeNCryptSession::WorkerEntry, this);
}

void VeNCryptSession::WorkerEntry() {
  SetSocketTimeouts(tcp_fd_.get(), kHandshakeTimeoutSeconds);
  const bool handshake_ok = DoHandshake();
  SetSocketTimeouts(tcp_fd_.get(), 0);  // relay phase: no deadline
  if (!handshake_ok) {
    PostFinished();
    return;
  }
  // Hand off the libvncserver-side fd on the main thread, then relay.
  base::ScopedFD remote = std::move(plain_remote_fd_);
  main_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(std::move(on_authenticated_), std::move(remote),
                     peer_ip_));
  RelayLoop();
  PostFinished();
}

bool VeNCryptSession::ReadExact(void* buf, size_t n) {
  return ReadAllFromFd(tcp_fd_.get(), buf, n);
}

bool VeNCryptSession::WriteExact(const void* buf, size_t n) {
  return WriteAllToFd(tcp_fd_.get(), buf, n);
}

bool VeNCryptSession::TlsReadExact(void* buf, size_t n) {
  uint8_t* p = static_cast<uint8_t*>(buf);
  while (n > 0) {
    int r = SSL_read(ssl_, p, static_cast<int>(n));
    if (r > 0) {
      p += r;
      n -= static_cast<size_t>(r);
      continue;
    }
    int err = SSL_get_error(ssl_, r);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
      continue;
    }
    LogSslErr("SSL_read");
    return false;
  }
  return true;
}

bool VeNCryptSession::TlsWriteExact(const void* buf, size_t n) {
  const uint8_t* p = static_cast<const uint8_t*>(buf);
  while (n > 0) {
    int w = SSL_write(ssl_, p, static_cast<int>(n));
    if (w > 0) {
      p += w;
      n -= static_cast<size_t>(w);
      continue;
    }
    int err = SSL_get_error(ssl_, w);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
      continue;
    }
    LogSslErr("SSL_write");
    return false;
  }
  return true;
}

bool VeNCryptSession::DoVncAuthHandshake() {
  // RFB 3.8 VncAuth: server sends 16 random bytes, client encrypts with
  // password (DES, bit-reversed key) and sends back 16 bytes; server
  // verifies and sends SecurityResult (u32; 0=OK, 1=fail).
  uint8_t challenge[16];
  if (RAND_bytes(challenge, sizeof(challenge)) != 1) {
    LOG(WARNING) << "VncAuth: RAND_bytes failed";
    return false;
  }
  if (!WriteExact(challenge, sizeof(challenge))) {
    return false;
  }
  uint8_t response[16];
  if (!ReadExact(response, sizeof(response))) {
    return false;
  }
  bool ok = VerifyVncAuthResponse(password_plain_, challenge, response);
  uint32_t result_be = htonl(ok ? 0u : 1u);
  if (!WriteExact(&result_be, 4)) {
    return false;
  }
  if (!ok) {
    LOG(WARNING) << "VncAuth: bad password for " << peer_ip_;
    return false;
  }
  LOG(INFO) << "VncAuth: auth OK for " << peer_ip_
            << " (insecure mode; RFB traffic is PLAINTEXT)";

  // Set up the socketpair the same way the VeNCrypt branch does, so
  // RelayLoop runs uniformly. ssl_ stays null → relay copies bytes
  // straight through.
  int sp[2] = {-1, -1};
  if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sp) != 0) {
    PLOG(WARNING) << "VncAuth: socketpair";
    return false;
  }
  plain_local_fd_.reset(sp[0]);
  plain_remote_fd_.reset(sp[1]);
  return true;
}

bool VeNCryptSession::DoHandshake() {
  // --- 1. RFB version exchange (over plain TCP) ---
  if (!WriteExact(kRfbProtoVersion, kRfbVersionLen)) {
    return false;
  }
  char client_ver[kRfbVersionLen];
  if (!ReadExact(client_ver, kRfbVersionLen)) {
    return false;
  }
  // We accept any "RFB 003.00X" — the client's stated version doesn't
  // affect VeNCrypt negotiation.

  // --- 2. Advertise security types ---
  // Loopback + no password → advertise sec-type None (1) so noVNC
  // does not display a spurious credential prompt (its Plain sub-type
  // path unconditionally requires username+password). Loopback here is
  // implicitly trusted: only the on-device websockify proxy can reach
  // 127.0.0.1:5900. The outer wss:// terminates encryption already.
  const bool is_loopback_early = (peer_ip_ == "127.0.0.1");
  const bool password_required_early = !password_hash_.empty();
  if (is_loopback_early && !password_required_early) {
    uint8_t adv_none[2] = {1, kSecTypeNone};
    if (!WriteExact(adv_none, 2)) {
      return false;
    }
    uint8_t chosen_none = 0;
    if (!ReadExact(&chosen_none, 1)) {
      return false;
    }
    if (chosen_none != kSecTypeNone) {
      LOG(WARNING) << "loopback: client picked unexpected security "
                   << +chosen_none;
      return false;
    }
    uint32_t result_be = htonl(0u);
    if (!WriteExact(&result_be, 4)) {
      return false;
    }
    LOG(INFO) << "loopback: SecTypeNone (no password) accepted for "
              << peer_ip_;
    int sp[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sp) != 0) {
      PLOG(WARNING) << "loopback SecTypeNone: socketpair";
      return false;
    }
    plain_local_fd_.reset(sp[0]);
    plain_remote_fd_.reset(sp[1]);
    return true;
  }
  // VeNCrypt is always offered. VncAuth (insecure, plaintext RFB) is
  // only added when "allow insecure clients" mode is on AND a plaintext
  // password mirror is available.
  const bool allow_vnc_auth = !password_plain_.empty();
  // Advertisement buffer: [count, type...].
  uint8_t adv[3];
  uint8_t adv_count = 1;
  adv[1] = kSecTypeVeNCrypt;
  if (allow_vnc_auth) {
    adv[++adv_count] = kSecTypeVncAuth;
  }
  adv[0] = adv_count;
  if (!WriteExact(adv, 1 + adv_count)) {
    return false;
  }
  uint8_t chosen = 0;
  if (!ReadExact(&chosen, 1)) {
    return false;
  }
  if (chosen == kSecTypeVncAuth && allow_vnc_auth) {
    return DoVncAuthHandshake();
  }
  if (chosen != kSecTypeVeNCrypt) {
    LOG(WARNING) << "VeNCrypt: client picked unexpected security " << +chosen;
    return false;
  }

  // --- 3. VeNCrypt sub-handshake: version, sub-types ---
  uint8_t vn_ver[2] = {kVeNCryptMajor, kVeNCryptMinor};
  if (!WriteExact(vn_ver, 2)) {
    return false;
  }
  uint8_t cli_ver[2];
  if (!ReadExact(cli_ver, 2)) {
    return false;
  }
  // We require 0.2; if the client offers an older version, reject.
  uint8_t version_ok = (cli_ver[0] == kVeNCryptMajor &&
                       cli_ver[1] >= kVeNCryptMinor)
                          ? 0
                          : 1;
  if (!WriteExact(&version_ok, 1)) {
    return false;
  }
  if (version_ok != 0) {
    LOG(WARNING) << "VeNCrypt: client requested version " << +cli_ver[0] << "."
                 << +cli_ver[1] << " not supported";
    return false;
  }

  // Sub-type advertisement depends on whether the peer is the on-device
  // noVNC wsproxy (loopback) or a real LAN RFB client.
  //
  // - LAN peers must speak our X509 sub-handshake (RFB-embedded TLS
  //   with our self-signed cert). Real VNC viewers (TigerVNC, etc.)
  //   do this natively. We advertise X509Plain when a password is set
  //   so an attacker can't downgrade to X509None.
  //
  // - Loopback peers come from the websockify sidecar, which proxies
  //   the browser's wss://device:6080 to plain TCP on 127.0.0.1:5900.
  //   The outer WebSocket-over-TLS already terminates encryption at
  //   the device, so re-doing TLS inside the RFB stream is redundant.
  //   noVNC's JS-side RFB.js (≤ v1.2) also does not implement the
  //   X509 sub-handshake — it only knows Plain (sub-type 256). We
  //   advertise Plain on loopback so the browser path can actually
  //   complete authentication. With no password set, any credentials
  //   the client sends are accepted; with a password set, we still
  //   verify against the bcrypt hash.
  const bool is_loopback = (peer_ip_ == "127.0.0.1");
  const bool password_required = !password_hash_.empty();
  uint8_t num_subtypes = 1;
  if (!WriteExact(&num_subtypes, 1)) {
    return false;
  }
  uint32_t sub_be;
  if (is_loopback) {
    sub_be = htonl(kSubTypePlain);
  } else {
    sub_be = htonl(password_required ? kSubTypeX509Plain : kSubTypeX509None);
  }
  if (!WriteExact(&sub_be, 4)) {
    return false;
  }
  uint32_t cli_sub_be = 0;
  if (!ReadExact(&cli_sub_be, 4)) {
    return false;
  }
  uint32_t cli_sub = ntohl(cli_sub_be);
  // For loopback we only ever advertised Plain; for LAN we only ever
  // advertised X509{None,Plain}, and additionally forbid downgrades
  // from X509Plain to X509None when a password is set.
  const bool subtype_ok =
      is_loopback ? (cli_sub == kSubTypePlain)
                  : ((cli_sub == kSubTypeX509Plain) ||
                     (cli_sub == kSubTypeX509None && !password_required));
  // Real VNC clients (TigerVNC, libvncclient) expect a 1-byte
  // accept/reject after picking a sub-type. noVNC ≤ 1.2's Plain path
  // does NOT read this byte — after sending its sub-type choice it
  // immediately sends the credential length fields. So on the loopback
  // path we skip the accept byte. On failure the reject byte is sent
  // unconditionally — the connection is going down anyway, and a
  // loopback client that doesn't read it is unaffected.
  if (!subtype_ok) {
    uint8_t sub_status = 0;
    WriteExact(&sub_status, 1);
    LOG(WARNING) << "VeNCrypt: client picked unsupported sub-type " << cli_sub
                 << " (password_required=" << password_required << ")";
    return false;
  }
  if (!is_loopback) {
    uint8_t sub_status = 1;
    if (!WriteExact(&sub_status, 1)) {
      return false;
    }
  }
  // --- 4a. Loopback Plain credentials over raw TCP (no inner TLS).
  //         The outer wss:// (websockify) already terminates encryption,
  //         and noVNC's JS RFB stack can't speak X509 anyway, so we do
  //         the Plain (256) credential exchange and security result
  //         straight on the unencrypted socket. Then fall through to
  //         the socketpair / RelayLoop wiring without an SSL context.
  if (is_loopback) {
    uint32_t ulen_be = 0, plen_be = 0;
    if (!ReadExact(&ulen_be, 4)) {
      return false;
    }
    if (!ReadExact(&plen_be, 4)) {
      return false;
    }
    uint32_t ulen = ntohl(ulen_be);
    uint32_t plen = ntohl(plen_be);
    if (ulen > 4096 || plen > 4096) {
      LOG(WARNING) << "VeNCrypt Plain (loopback): oversized credentials "
                   << ulen << "/" << plen;
      return false;
    }
    std::string username(ulen, '\0');
    std::string password(plen, '\0');
    if (ulen && !ReadExact(username.data(), ulen)) {
      return false;
    }
    if (plen && !ReadExact(password.data(), plen)) {
      return false;
    }
    // When no password is configured, accept any credentials — the
    // loopback path is implicitly trusted (only the on-device wsproxy
    // can reach 127.0.0.1:5900). When a password is configured,
    // validate against the bcrypt hash exactly as the X509Plain path
    // does.
    bool ok = password_hash_.empty() ||
              VerifyPassword(password, password_hash_);
    uint32_t result_be = htonl(ok ? 0u : 1u);
    if (!WriteExact(&result_be, 4)) {
      return false;
    }
    if (!ok) {
      static constexpr char kReason[] = "Password incorrect";
      uint32_t rlen_be = htonl(static_cast<uint32_t>(sizeof(kReason) - 1));
      WriteExact(&rlen_be, 4);
      WriteExact(kReason, sizeof(kReason) - 1);
      LOG(WARNING) << "VeNCrypt Plain (loopback): auth failed for "
                   << peer_ip_;
      return false;
    }
    LOG(INFO) << "VeNCrypt Plain (loopback): auth OK for " << peer_ip_;

    int sp[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sp) != 0) {
      PLOG(WARNING) << "VeNCrypt Plain (loopback): socketpair";
      return false;
    }
    plain_local_fd_.reset(sp[0]);
    plain_remote_fd_.reset(sp[1]);
    return true;
  }

  const bool is_plain = (cli_sub == kSubTypeX509Plain);

  // --- 4. TLS handshake (server) ---
  ssl_ = SSL_new(ssl_ctx_);
  if (!ssl_) {
    LogSslErr("SSL_new");
    return false;
  }
  if (SSL_set_fd(ssl_, tcp_fd_.get()) != 1) {
    LogSslErr("SSL_set_fd");
    return false;
  }
  int rc = SSL_accept(ssl_);
  if (rc != 1) {
    int err = SSL_get_error(ssl_, rc);
    LOG(WARNING) << "VeNCrypt: SSL_accept failed err=" << err;
    LogSslErr("SSL_accept");
    return false;
  }

  // --- 5. X509Plain credentials (only when negotiated) ---
  if (is_plain) {
    uint32_t ulen_be = 0, plen_be = 0;
    if (!TlsReadExact(&ulen_be, 4)) {
      return false;
    }
    if (!TlsReadExact(&plen_be, 4)) {
      return false;
    }
    uint32_t ulen = ntohl(ulen_be);
    uint32_t plen = ntohl(plen_be);
    // Reasonable cap to thwart adversarial sizes.
    if (ulen > 4096 || plen > 4096) {
      LOG(WARNING) << "VeNCrypt Plain: oversized username/password lengths "
                   << ulen << "/" << plen;
      return false;
    }
    std::string username(ulen, '\0');
    std::string password(plen, '\0');
    if (ulen && !TlsReadExact(username.data(), ulen)) {
      return false;
    }
    if (plen && !TlsReadExact(password.data(), plen)) {
      return false;
    }
    // We ignore |username| (single-user device). Validate password
    // against the stored bcrypt hash.
    bool ok = VerifyPassword(password, password_hash_);
    uint32_t result_be = htonl(ok ? 0u : 1u);
    if (!TlsWriteExact(&result_be, 4)) {
      return false;
    }
    if (!ok) {
      // RFB 3.8 mandates a reason string on failure.
      static constexpr char kReason[] = "Password incorrect";
      uint32_t rlen_be = htonl(static_cast<uint32_t>(sizeof(kReason) - 1));
      TlsWriteExact(&rlen_be, 4);
      TlsWriteExact(kReason, sizeof(kReason) - 1);
      LOG(WARNING) << "VeNCrypt Plain: auth failed for " << peer_ip_;
      return false;
    }
    LOG(INFO) << "VeNCrypt Plain: auth OK for " << peer_ip_;
  } else {
    // --- 5. SecurityResult over TLS: u32 (0 = OK) for X509None ---
    uint32_t ok = 0;
    if (!TlsWriteExact(&ok, 4)) {
      return false;
    }
  }

  // --- 6. Pre-feed libvncserver: it expects a "ClientInit" byte next
  //     from the wire. With our socketpair architecture, the byte the
  //     client sends over TLS hits us in RelayLoop, gets forwarded to
  //     the plain socket, and libvncserver consumes it from there.

  // Create the socketpair that bridges TLS ↔ libvncserver.
  int sp[2] = {-1, -1};
  if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sp) != 0) {
    PLOG(WARNING) << "VeNCrypt: socketpair";
    return false;
  }
  plain_local_fd_.reset(sp[0]);
  plain_remote_fd_.reset(sp[1]);
  LOG(INFO) << "VeNCrypt: TLS handshake complete with " << peer_ip_;
  return true;
}

void VeNCryptSession::RelayLoop() {
  // Two-way relay. We poll both the TCP socket (for OpenSSL data) and
  // the plain end of the socketpair (libvncserver writes). For each
  // direction we maintain a buffer of pending bytes and only re-arm
  // POLLIN when the buffer is empty.
  constexpr size_t kBuf = 32 * 1024;
  std::vector<uint8_t> tx_buf(kBuf);  // bytes to push to TLS
  size_t tx_have = 0;
  std::vector<uint8_t> rx_buf(kBuf);  // bytes to push to libvncserver
  size_t rx_have = 0;

  // libvncserver's rfbNewClient unconditionally writes the 12-byte
  // "RFB 003.008\n" protocol-version banner to its socket end. With
  // our socketpair architecture that lands here as the first chunk on
  // |plain_local_fd_|. We drop it instead of forwarding to the client,
  // because the VeNCrypt path already completed RFB version
  // negotiation on plain TCP before TLS came up.
  size_t suppress_tx = 12;

  while (!stop_flag_.load()) {
    pollfd pfd[2];
    pfd[0].fd = tcp_fd_.get();
    pfd[0].events = (rx_have == 0 ? POLLIN : 0) | (tx_have > 0 ? POLLOUT : 0);
    pfd[1].fd = plain_local_fd_.get();
    pfd[1].events = (tx_have == 0 ? POLLIN : 0) | (rx_have > 0 ? POLLOUT : 0);

    if (poll(pfd, 2, /*timeout=*/-1) < 0) {
      if (errno == EINTR) {
        continue;
      }
      PLOG(WARNING) << "VeNCrypt relay: poll";
      break;
    }

    // libvncserver → TLS (read plain, write SSL).
    if ((pfd[1].revents & POLLIN) && tx_have == 0) {
      ssize_t r = read(plain_local_fd_.get(), tx_buf.data(), tx_buf.size());
      if (r > 0) {
        tx_have = static_cast<size_t>(r);
        if (suppress_tx > 0) {
          size_t drop = std::min(suppress_tx, tx_have);
          if (drop < tx_have) {
            std::memmove(tx_buf.data(), tx_buf.data() + drop, tx_have - drop);
          }
          tx_have -= drop;
          suppress_tx -= drop;
        }
      } else if (r == 0) {
        break;
      } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
        break;
      }
    }
    if (tx_have > 0) {
      ssize_t w = ssl_ ? static_cast<ssize_t>(SSL_write(
                              ssl_, tx_buf.data(),
                              static_cast<int>(tx_have)))
                       : write(tcp_fd_.get(), tx_buf.data(), tx_have);
      if (w > 0) {
        if (static_cast<size_t>(w) < tx_have) {
          std::memmove(tx_buf.data(), tx_buf.data() + w, tx_have - w);
        }
        tx_have -= static_cast<size_t>(w);
      } else if (ssl_) {
        int err = SSL_get_error(ssl_, static_cast<int>(w));
        if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) {
          LogSslErr("SSL_write relay");
          break;
        }
      } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
        break;
      }
    }

    // TLS → libvncserver (read SSL/plain, write plain).
    if ((pfd[0].revents & POLLIN) && rx_have == 0) {
      ssize_t r = ssl_ ? static_cast<ssize_t>(SSL_read(
                              ssl_, rx_buf.data(),
                              static_cast<int>(rx_buf.size())))
                       : read(tcp_fd_.get(), rx_buf.data(), rx_buf.size());
      if (r > 0) {
        rx_have = static_cast<size_t>(r);
      } else if (r == 0 && !ssl_) {
        break;
      } else if (ssl_) {
        int err = SSL_get_error(ssl_, static_cast<int>(r));
        if (err == SSL_ERROR_ZERO_RETURN) {
          break;
        }
        if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) {
          LogSslErr("SSL_read relay");
          break;
        }
      } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
        break;
      }
    }
    if (rx_have > 0) {
      ssize_t w =
          write(plain_local_fd_.get(), rx_buf.data(), rx_have);
      if (w > 0) {
        if (static_cast<size_t>(w) < rx_have) {
          std::memmove(rx_buf.data(), rx_buf.data() + w, rx_have - w);
        }
        rx_have -= static_cast<size_t>(w);
      } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
        break;
      }
    }

    if ((pfd[0].revents & (POLLERR | POLLHUP)) ||
        (pfd[1].revents & (POLLERR | POLLHUP))) {
      break;
    }
  }
  LOG(INFO) << "VeNCrypt: relay ended for " << peer_ip_;
}

void VeNCryptSession::PostFinished() {
  if (on_finished_) {
    main_runner_->PostTask(FROM_HERE,
                           base::BindOnce(std::move(on_finished_), this));
  }
}

}  // namespace fydeos::vnc::net
