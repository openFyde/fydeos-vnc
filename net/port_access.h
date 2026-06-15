// Copyright 2026 The FydeOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FYDEOS_VNC_NET_PORT_ACCESS_H_
#define FYDEOS_VNC_NET_PORT_ACCESS_H_

#include <cstdint>
#include <memory>
#include <string>

#include <base/files/scoped_file.h>
#include <base/memory/scoped_refptr.h>
#include <dbus/bus.h>

namespace fydeos::vnc::net {

// RAII helper that asks ChromeOS's permission_broker to open a TCP port
// on a specific interface, and releases it on destruction (or on daemon
// crash, since the broker watches our end of the lifeline pipe for EOF).
//
// Use:
//   auto access = PortAccess::Request(bus, 5900, "wlan0");
//   if (!access) { ...failed... }
//   // port stays open until |access| destroyed
class PortAccess {
 public:
  // Returns nullptr on failure (broker rejected, D-Bus error, etc.).
  static std::unique_ptr<PortAccess> Request(dbus::Bus* bus,
                                              uint16_t port,
                                              const std::string& iface);
  ~PortAccess();

  PortAccess(const PortAccess&) = delete;
  PortAccess& operator=(const PortAccess&) = delete;

  uint16_t port() const { return port_; }
  const std::string& iface() const { return iface_; }

 private:
  PortAccess(scoped_refptr<dbus::Bus> bus,
             uint16_t port,
             std::string iface,
             base::ScopedFD lifeline_local);

  scoped_refptr<dbus::Bus> bus_;
  const uint16_t port_;
  const std::string iface_;
  // Daemon-side end of the lifeline pipe; when this fd closes (here at
  // destruction, or via daemon process exit), the broker sees EOF on its
  // end and tears down the iptables ACCEPT rule.
  base::ScopedFD lifeline_local_;
};

}  // namespace fydeos::vnc::net

#endif  // FYDEOS_VNC_NET_PORT_ACCESS_H_
