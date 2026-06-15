// Copyright 2026 The FydeOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FYDEOS_VNC_NET_MDNS_H_
#define FYDEOS_VNC_NET_MDNS_H_

#include <memory>
#include <string>

namespace dbus {
class Bus;
class ObjectProxy;
}  // namespace dbus

namespace fydeos::vnc::net {

// Publishes "_rfb._tcp.local." via the avahi-daemon's D-Bus API. The
// previous implementation wrote a service XML file to
// /etc/avahi/services/, but ChromeOS rootfs is read-only at runtime so
// the write silently failed. Talking to avahi-daemon directly works
// regardless of filesystem layout.
//
// One publication per process. Constructing the object adds + commits
// the entry; destruction frees the entry group and the service goes
// away. Caller passes the system bus (already owned by the daemon).
class MdnsPublication {
 public:
  // Returns nullptr if the publish round-trip fails (avahi-daemon not
  // running, D-Bus policy denied, etc.). The daemon should treat that
  // as a non-fatal warning — VNC still works, clients just won't find
  // the host by browsing mDNS.
  static std::unique_ptr<MdnsPublication> Publish(dbus::Bus* bus,
                                                  int port,
                                                  const std::string& model);

  MdnsPublication(const MdnsPublication&) = delete;
  MdnsPublication& operator=(const MdnsPublication&) = delete;
  ~MdnsPublication();

 private:
  explicit MdnsPublication(dbus::ObjectProxy* group_proxy);

  dbus::ObjectProxy* group_proxy_;  // not owned (bus owns)
};

}  // namespace fydeos::vnc::net

#endif  // FYDEOS_VNC_NET_MDNS_H_
