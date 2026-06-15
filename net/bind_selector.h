// Copyright 2026 The FydeOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FYDEOS_VNC_NET_BIND_SELECTOR_H_
#define FYDEOS_VNC_NET_BIND_SELECTOR_H_

#include <cstdint>
#include <string>
#include <vector>

namespace fydeos::vnc::net {

struct LanInterface {
  std::string name;     // e.g. "eth0", "wlan0"
  std::string ipv4;     // dotted-quad string; may be empty
  std::string ipv6;     // colon-form string; may be empty
};

// Enumerate "LAN-class" interfaces — those with an RFC-1918 IPv4 address
// (10/8, 172.16/12, 192.168/16, 169.254/16) or a link-local/ULA IPv6
// address (fe80::/10, fc00::/7). Excludes loopback by default and any
// interface whose name starts with "tun"/"tap" (VPN tunnels) — design
// §5.7.1.
std::vector<LanInterface> EnumerateLanInterfaces();

// Returns the IPv4 address suitable for libvncserver's listenInterface
// field, in host byte order. Picks the first matching ethernet/Wi-Fi
// interface. Returns 0 if no LAN interface is present (in which case the
// caller should keep the listening socket down — design §2.4 NF-4).
uint32_t SelectIPv4BindAddress();

// Returns the name of the same iface SelectIPv4BindAddress() picks (e.g.,
// "wlan0"). Used for permission_broker's per-interface port allow.
std::string SelectIPv4BindInterface();

}  // namespace fydeos::vnc::net

#endif  // FYDEOS_VNC_NET_BIND_SELECTOR_H_
