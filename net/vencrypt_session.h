// Copyright 2026 The FydeOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FYDEOS_VNC_NET_VENCRYPT_SESSION_H_
#define FYDEOS_VNC_NET_VENCRYPT_SESSION_H_

#include <atomic>
#include <memory>
#include <string>
#include <thread>

#include <base/files/scoped_file.h>
#include <base/functional/callback.h>
#include <base/memory/scoped_refptr.h>
#include <base/task/single_thread_task_runner.h>
#include <openssl/ssl.h>

namespace fydeos::vnc::net {

// Per-connection VeNCrypt + X.509 TLS terminator. Owns the accepted
// TCP socket, performs the VeNCrypt handshake + TLS upgrade on a
// dedicated worker thread, then becomes a relay between the TLS
// socket and one end of a socketpair. The other end is handed back
// to the daemon's main thread (via |on_authenticated|) where it gets
// passed to libvncserver via rfbNewClient.
//
// The session self-destructs when either end closes. Caller keeps a
// std::unique_ptr in a list and removes it when notified via
// |on_finished|.
class VeNCryptSession {
 public:
  // Called on the main task runner once VeNCrypt+TLS auth has completed
  // successfully. |plain_fd| is one end of the socketpair; libvncserver
  // reads/writes plain RFB bytes on it. Ownership transfers.
  using AuthSuccessCb =
      base::OnceCallback<void(base::ScopedFD plain_fd, std::string peer_ip)>;
  // Called on the main task runner when the session has fully ended
  // (handshake failed OR relay loop saw EOF / error). Receives a raw
  // self-pointer so the owner can locate and delete this object.
  using FinishedCb = base::OnceCallback<void(VeNCryptSession* self)>;

  // |password_hash| is the bcrypt-hashed password (or empty for no
  // password). When non-empty, the server additionally advertises the
  // X509Plain sub-type and requires the client to supply a matching
  // password over the TLS tunnel before the connection is handed to
  // libvncserver.
  // |password_plain| is the plaintext password mirror used for the
  // legacy VncAuth security type when "allow insecure clients" mode
  // is on. Empty means VncAuth is NOT advertised. Required because
  // VncAuth challenge-response cannot be implemented over bcrypt.
  VeNCryptSession(base::ScopedFD tcp_fd,
                  std::string peer_ip,
                  SSL_CTX* ssl_ctx,
                  std::string password_hash,
                  std::string password_plain,
                  AuthSuccessCb on_authenticated,
                  FinishedCb on_finished);
  VeNCryptSession(const VeNCryptSession&) = delete;
  VeNCryptSession& operator=(const VeNCryptSession&) = delete;
  ~VeNCryptSession();

  // Spawn the worker thread that drives handshake + relay. Returns
  // immediately; |on_authenticated| / |on_finished| fire later.
  void Start();

 private:
  void WorkerEntry();
  // Returns true on success. On failure |tcp_fd_| is closed.
  bool DoHandshake();
  // Inner branch invoked by DoHandshake when the client picks VncAuth
  // instead of VeNCrypt. Returns true if auth succeeded and the
  // socketpair is ready for relay.
  bool DoVncAuthHandshake();
  // Relay between TLS socket and |plain_remote_fd_| until either side
  // closes. Returns when done. When |ssl_| is null (VncAuth path),
  // bytes are forwarded without encryption.
  void RelayLoop();
  void PostFinished();

  // Buffered TCP IO during the plain-text VeNCrypt sub-handshake.
  bool ReadExact(void* buf, size_t n);
  bool WriteExact(const void* buf, size_t n);
  // Same, but after TLS is established.
  bool TlsReadExact(void* buf, size_t n);
  bool TlsWriteExact(const void* buf, size_t n);

  // Inputs (immutable after construction).
  base::ScopedFD tcp_fd_;
  std::string peer_ip_;
  SSL_CTX* const ssl_ctx_;
  std::string password_hash_;
  std::string password_plain_;
  scoped_refptr<base::SingleThreadTaskRunner> main_runner_;
  AuthSuccessCb on_authenticated_;
  FinishedCb on_finished_;

  // Worker-thread state.
  std::thread worker_;
  SSL* ssl_ = nullptr;
  // Socketpair: |plain_local_fd_| stays with us; |plain_remote_fd_|
  // is handed to libvncserver after auth.
  base::ScopedFD plain_local_fd_;
  base::ScopedFD plain_remote_fd_;
  std::atomic<bool> stop_flag_{false};
};

}  // namespace fydeos::vnc::net

#endif  // FYDEOS_VNC_NET_VENCRYPT_SESSION_H_
