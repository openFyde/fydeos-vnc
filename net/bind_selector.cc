// Copyright 2026 The FydeOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "fydeos-vnc/net/bind_selector.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>

#include <map>

#include <base/logging.h>

namespace fydeos::vnc::net {

namespace {

bool IsPrivateIPv4(uint32_t host_be) {
  const uint32_t addr = ntohl(host_be);
  // 10.0.0.0/8
  if ((addr & 0xff000000) == 0x0a000000) {
    return true;
  }
  // 172.16.0.0/12
  if ((addr & 0xfff00000) == 0xac100000) {
    return true;
  }
  // 192.168.0.0/16
  if ((addr & 0xffff0000) == 0xc0a80000) {
    return true;
  }
  // 169.254.0.0/16 link-local
  if ((addr & 0xffff0000) == 0xa9fe0000) {
    return true;
  }
  return false;
}

bool IsPrivateIPv6(const in6_addr& a) {
  // fc00::/7 ULA
  if ((a.s6_addr[0] & 0xfe) == 0xfc) {
    return true;
  }
  // fe80::/10 link-local
  if (a.s6_addr[0] == 0xfe && (a.s6_addr[1] & 0xc0) == 0x80) {
    return true;
  }
  return false;
}

bool IsExcludedIfaceName(const std::string& name) {
  return name == "lo" || name.starts_with("tun") || name.starts_with("tap") ||
         name.starts_with("docker") || name.starts_with("veth") ||
         name.starts_with("br-");
}

}  // namespace

std::vector<LanInterface> EnumerateLanInterfaces() {
  std::vector<LanInterface> out;
  ifaddrs* head = nullptr;
  if (getifaddrs(&head) != 0) {
    PLOG(ERROR) << "getifaddrs";
    return out;
  }

  std::map<std::string, LanInterface> by_name;
  for (ifaddrs* it = head; it; it = it->ifa_next) {
    if (!it->ifa_addr || !it->ifa_name) {
      continue;
    }
    if (!(it->ifa_flags & IFF_UP) || (it->ifa_flags & IFF_LOOPBACK)) {
      continue;
    }
    const std::string name = it->ifa_name;
    if (IsExcludedIfaceName(name)) {
      continue;
    }

    char buf[INET6_ADDRSTRLEN] = {0};
    if (it->ifa_addr->sa_family == AF_INET) {
      auto* sa = reinterpret_cast<sockaddr_in*>(it->ifa_addr);
      if (!IsPrivateIPv4(sa->sin_addr.s_addr)) {
        continue;
      }
      inet_ntop(AF_INET, &sa->sin_addr, buf, sizeof(buf));
      by_name[name].name = name;
      by_name[name].ipv4 = buf;
    } else if (it->ifa_addr->sa_family == AF_INET6) {
      auto* sa = reinterpret_cast<sockaddr_in6*>(it->ifa_addr);
      if (!IsPrivateIPv6(sa->sin6_addr)) {
        continue;
      }
      inet_ntop(AF_INET6, &sa->sin6_addr, buf, sizeof(buf));
      by_name[name].name = name;
      by_name[name].ipv6 = buf;
    }
  }
  freeifaddrs(head);

  for (auto& [_, iface] : by_name) {
    out.push_back(std::move(iface));
  }
  return out;
}

uint32_t SelectIPv4BindAddress() {
  auto ifaces = EnumerateLanInterfaces();
  for (const auto& i : ifaces) {
    if (i.ipv4.empty()) {
      continue;
    }
    in_addr a{};
    if (inet_pton(AF_INET, i.ipv4.c_str(), &a) == 1) {
      LOG(INFO) << "Binding RFB to " << i.name << " (" << i.ipv4 << ")";
      return a.s_addr;  // network byte order; libvncserver takes it as-is
    }
  }
  return 0;
}

std::string SelectIPv4BindInterface() {
  for (const auto& i : EnumerateLanInterfaces()) {
    if (!i.ipv4.empty()) {
      return i.name;
    }
  }
  return {};
}

}  // namespace fydeos::vnc::net
