// Copyright 2026 The FydeOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FYDEOS_VNC_NET_TCP_LISTENER_H_
#define FYDEOS_VNC_NET_TCP_LISTENER_H_

#include <netinet/in.h>

#include <cstdint>
#include <memory>
#include <string>

#include <base/files/file_descriptor_watcher_posix.h>
#include <base/files/scoped_file.h>
#include <base/functional/callback.h>

namespace fydeos::vnc::net {

// Owns the daemon's RFB TCP listening sockets (one IPv4 + one IPv6) and
// fires |on_accept| on the calling sequence whenever a client connects.
// M4-bis-a moves the listen socket out of libvncserver so we can do
// VeNCrypt / TLS termination in-process before handing a (later: post-
// TLS) FD to libvncserver via rfbNewClient.
class TcpListener {
 public:
  // peer_ip is a printable address ("192.168.0.20" / "fe80::...") for
  // logging and libvncserver's cl->host field. fd ownership transfers
  // to the callback; close it if you don't want it.
  using AcceptCallback =
      base::RepeatingCallback<void(base::ScopedFD fd, std::string peer_ip)>;

  TcpListener();
  TcpListener(const TcpListener&) = delete;
  TcpListener& operator=(const TcpListener&) = delete;
  ~TcpListener();

  // Bind to the chosen LAN IPv4 (network byte order; 0 = INADDR_ANY) and
  // IPv6 ANY on |port|. Returns false on socket / bind / listen error.
  // |on_accept| must remain valid for the lifetime of this object.
  bool Start(uint32_t listen_ipv4_network_order,
             int port,
             AcceptCallback on_accept);
  void Stop();

 private:
  void OnIPv4Readable();
  void OnIPv6Readable();
  void OnLoopbackReadable();
  void DoAccept(int listen_fd);

  base::ScopedFD ipv4_fd_;
  base::ScopedFD ipv6_fd_;
  // M11-b: also bind 127.0.0.1 so the in-host noVNC websockify proxy
  // can reach the daemon without depending on the current DHCP-assigned
  // LAN IP. permission_broker doesn't need to open loopback — kernel
  // accepts on lo by default.
  base::ScopedFD loopback_fd_;
  std::unique_ptr<base::FileDescriptorWatcher::Controller> ipv4_watch_;
  std::unique_ptr<base::FileDescriptorWatcher::Controller> ipv6_watch_;
  std::unique_ptr<base::FileDescriptorWatcher::Controller> loopback_watch_;
  AcceptCallback on_accept_;
};

}  // namespace fydeos::vnc::net

#endif  // FYDEOS_VNC_NET_TCP_LISTENER_H_
