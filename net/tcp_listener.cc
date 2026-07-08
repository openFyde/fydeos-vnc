// Copyright 2026 The FydeOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "fydeos-vnc/net/tcp_listener.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <utility>

#include <base/functional/bind.h>
#include <base/logging.h>

namespace fydeos::vnc::net {

namespace {

constexpr int kListenBacklog = 8;

// Create + bind + listen, returning an owned fd. Returns invalid on error.
base::ScopedFD MakeListenSocket(int family,
                                const sockaddr* addr,
                                socklen_t addrlen) {
  base::ScopedFD fd(socket(family, SOCK_STREAM | SOCK_CLOEXEC, 0));
  if (!fd.is_valid()) {
    PLOG(ERROR) << "socket(" << family << ")";
    return {};
  }
  int reuse = 1;
  setsockopt(fd.get(), SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
  if (family == AF_INET6) {
    int v6only = 1;  // Pair with separate AF_INET socket.
    setsockopt(fd.get(), IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));
  }
  if (bind(fd.get(), addr, addrlen) != 0) {
    PLOG(ERROR) << "bind(family=" << family << ")";
    return {};
  }
  if (listen(fd.get(), kListenBacklog) != 0) {
    PLOG(ERROR) << "listen";
    return {};
  }
  return fd;
}

std::string PrintablePeer(const sockaddr_storage& ss) {
  char buf[INET6_ADDRSTRLEN] = {};
  if (ss.ss_family == AF_INET) {
    auto* in = reinterpret_cast<const sockaddr_in*>(&ss);
    inet_ntop(AF_INET, &in->sin_addr, buf, sizeof(buf));
  } else if (ss.ss_family == AF_INET6) {
    auto* in6 = reinterpret_cast<const sockaddr_in6*>(&ss);
    inet_ntop(AF_INET6, &in6->sin6_addr, buf, sizeof(buf));
  }
  return buf;
}

}  // namespace

TcpListener::TcpListener() = default;
TcpListener::~TcpListener() {
  Stop();
}

bool TcpListener::Start(uint32_t listen_ipv4_network_order,
                        int port,
                        AcceptCallback on_accept) {
  on_accept_ = std::move(on_accept);

  // IPv4 — bind to the selected LAN IP (or INADDR_ANY for the first
  // start when no LAN iface is up yet).
  sockaddr_in v4{};
  v4.sin_family = AF_INET;
  v4.sin_port = htons(static_cast<uint16_t>(port));
  v4.sin_addr.s_addr = listen_ipv4_network_order;
  ipv4_fd_ = MakeListenSocket(AF_INET, reinterpret_cast<sockaddr*>(&v4),
                              sizeof(v4));
  if (!ipv4_fd_.is_valid()) {
    return false;
  }

  // IPv6 — bind to any. LAN-only source filtering exists for v4 only
  // (the dominant case); v6 exposure is still limited by the
  // permission_broker firewall rule scoped to the LAN interface.
  sockaddr_in6 v6{};
  v6.sin6_family = AF_INET6;
  v6.sin6_port = htons(static_cast<uint16_t>(port));
  v6.sin6_addr = in6addr_any;
  ipv6_fd_ = MakeListenSocket(AF_INET6, reinterpret_cast<sockaddr*>(&v6),
                              sizeof(v6));
  if (!ipv6_fd_.is_valid()) {
    ipv4_fd_.reset();
    return false;
  }

  // Loopback — the noVNC websockify sidecar lives on this host and
  // connects to 127.0.0.1:|port|. permission_broker doesn't gate
  // loopback, so this socket is reachable only from inside the
  // device, by design.
  sockaddr_in lo{};
  lo.sin_family = AF_INET;
  lo.sin_port = htons(static_cast<uint16_t>(port));
  lo.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  loopback_fd_ = MakeListenSocket(AF_INET, reinterpret_cast<sockaddr*>(&lo),
                                   sizeof(lo));
  if (!loopback_fd_.is_valid()) {
    ipv4_fd_.reset();
    ipv6_fd_.reset();
    return false;
  }

  ipv4_watch_ = base::FileDescriptorWatcher::WatchReadable(
      ipv4_fd_.get(),
      base::BindRepeating(&TcpListener::OnIPv4Readable, base::Unretained(this)));
  ipv6_watch_ = base::FileDescriptorWatcher::WatchReadable(
      ipv6_fd_.get(),
      base::BindRepeating(&TcpListener::OnIPv6Readable, base::Unretained(this)));
  loopback_watch_ = base::FileDescriptorWatcher::WatchReadable(
      loopback_fd_.get(),
      base::BindRepeating(&TcpListener::OnLoopbackReadable,
                          base::Unretained(this)));
  return true;
}

void TcpListener::Stop() {
  ipv4_watch_.reset();
  ipv6_watch_.reset();
  loopback_watch_.reset();
  ipv4_fd_.reset();
  ipv6_fd_.reset();
  loopback_fd_.reset();
  on_accept_.Reset();
}

void TcpListener::OnIPv4Readable() {
  DoAccept(ipv4_fd_.get());
}

void TcpListener::OnIPv6Readable() {
  DoAccept(ipv6_fd_.get());
}

void TcpListener::OnLoopbackReadable() {
  DoAccept(loopback_fd_.get());
}

void TcpListener::DoAccept(int listen_fd) {
  sockaddr_storage peer{};
  socklen_t plen = sizeof(peer);
  base::ScopedFD accepted(accept4(listen_fd,
                                   reinterpret_cast<sockaddr*>(&peer), &plen,
                                   SOCK_CLOEXEC));
  if (!accepted.is_valid()) {
    if (errno != EAGAIN && errno != EWOULDBLOCK) {
      PLOG(WARNING) << "accept4";
    }
    return;
  }
  // Disable Nagle — RFB messages are small + latency-sensitive.
  int nodelay = 1;
  setsockopt(accepted.get(), IPPROTO_TCP, TCP_NODELAY, &nodelay,
             sizeof(nodelay));
  on_accept_.Run(std::move(accepted), PrintablePeer(peer));
}

}  // namespace fydeos::vnc::net
