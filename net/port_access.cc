// Copyright 2026 The FydeOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "fydeos-vnc/net/port_access.h"

#include <fcntl.h>
#include <unistd.h>

#include <utility>

#include <base/logging.h>
#include <chromeos/dbus/service_constants.h>
#include <dbus/message.h>
#include <dbus/object_path.h>
#include <dbus/object_proxy.h>

namespace fydeos::vnc::net {

// static
std::unique_ptr<PortAccess> PortAccess::Request(dbus::Bus* bus,
                                                 uint16_t port,
                                                 const std::string& iface) {
  if (!bus) {
    return nullptr;
  }

  // Lifeline pipe. We hand the read end to the broker; broker watches it
  // for EOF and removes the iptables rule when our write end closes
  // (explicit destruction here or daemon process death).
  int fds[2] = {-1, -1};
  if (pipe(fds) != 0) {
    PLOG(ERROR) << "PortAccess: pipe()";
    return nullptr;
  }
  base::ScopedFD broker_read(fds[0]);
  base::ScopedFD local_write(fds[1]);

  dbus::ObjectProxy* proxy = bus->GetObjectProxy(
      ::permission_broker::kPermissionBrokerServiceName,
      dbus::ObjectPath(::permission_broker::kPermissionBrokerServicePath));
  if (!proxy) {
    LOG(ERROR) << "PortAccess: PermissionBroker proxy unavailable";
    return nullptr;
  }

  dbus::MethodCall call(::permission_broker::kPermissionBrokerInterface,
                        ::permission_broker::kRequestTcpPortAccess);
  dbus::MessageWriter w(&call);
  w.AppendUint16(port);
  w.AppendString(iface);
  w.AppendFileDescriptor(broker_read.get());

  auto resp = proxy->CallMethodAndBlock(&call, /*timeout_ms=*/5000)
                  .value_or(nullptr);
  if (!resp) {
    LOG(WARNING) << "PortAccess: no response from PermissionBroker (broker "
                    "down?)";
    return nullptr;
  }
  dbus::MessageReader r(resp.get());
  bool allowed = false;
  if (!r.PopBool(&allowed) || !allowed) {
    LOG(ERROR) << "PortAccess: broker denied port " << port << " on "
               << iface;
    return nullptr;
  }
  LOG(INFO) << "PortAccess: opened TCP " << port << " on "
            << (iface.empty() ? "any" : iface);
  return std::unique_ptr<PortAccess>(new PortAccess(
      scoped_refptr<dbus::Bus>(bus), port, iface, std::move(local_write)));
}

PortAccess::PortAccess(scoped_refptr<dbus::Bus> bus,
                       uint16_t port,
                       std::string iface,
                       base::ScopedFD lifeline_local)
    : bus_(std::move(bus)),
      port_(port),
      iface_(std::move(iface)),
      lifeline_local_(std::move(lifeline_local)) {}

PortAccess::~PortAccess() {
  // Closing lifeline_local_ signals broker → broker removes the iptables
  // rule. No explicit Release D-Bus call needed.
  if (lifeline_local_.is_valid()) {
    LOG(INFO) << "PortAccess: releasing TCP " << port_ << " on " << iface_;
  }
}

}  // namespace fydeos::vnc::net
