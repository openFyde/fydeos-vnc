// Copyright 2026 The FydeOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "fydeos-vnc/net/mdns.h"

#include <unistd.h>

#include <cstdint>
#include <string>
#include <vector>

#include <base/logging.h>
#include <base/strings/string_util.h>
#include <dbus/bus.h>
#include <dbus/message.h>
#include <dbus/object_path.h>
#include <dbus/object_proxy.h>

namespace fydeos::vnc::net {

namespace {

constexpr char kAvahiService[] = "org.freedesktop.Avahi";
constexpr char kAvahiServerIface[] = "org.freedesktop.Avahi.Server";
constexpr char kAvahiEntryGroupIface[] = "org.freedesktop.Avahi.EntryGroup";
constexpr char kAvahiServerPath[] = "/";
// Special values per /usr/include/avahi-common/address.h:
//   AVAHI_IF_UNSPEC   = -1   (publish on all interfaces — avahi will
//                              limit to those with reachable v4/v6 anyway)
//   AVAHI_PROTO_UNSPEC = -1
constexpr int32_t kAvahiIfUnspec = -1;
constexpr int32_t kAvahiProtoUnspec = -1;
// Reply timeout for avahi calls. Avahi-daemon is responsive (it serves
// from in-memory state) so 2 s is generous.
constexpr int kAvahiTimeoutMs = 2000;

// Each TXT entry is encoded as a single byte array in avahi's
// AddService signature (array of array of bytes).
void AppendTxtEntries(dbus::MessageWriter* w,
                      const std::vector<std::string>& entries) {
  dbus::MessageWriter aay(nullptr);
  w->OpenArray("ay", &aay);
  for (const std::string& e : entries) {
    dbus::MessageWriter ay(nullptr);
    aay.OpenArray("y", &ay);
    for (char c : e) {
      ay.AppendByte(static_cast<uint8_t>(c));
    }
    aay.CloseContainer(&ay);
  }
  w->CloseContainer(&aay);
}

}  // namespace

// static
std::unique_ptr<MdnsPublication> MdnsPublication::Publish(
    dbus::Bus* bus, int port, const std::string& model) {
  if (!bus) {
    return nullptr;
  }

  dbus::ObjectProxy* server = bus->GetObjectProxy(
      kAvahiService, dbus::ObjectPath(kAvahiServerPath));

  // 1) EntryGroupNew → returns the object path of a fresh entry group.
  dbus::ObjectPath group_path;
  {
    dbus::MethodCall call(kAvahiServerIface, "EntryGroupNew");
    auto resp = server->CallMethodAndBlock(&call, kAvahiTimeoutMs);
    if (!resp.has_value() || !resp.value()) {
      LOG(WARNING) << "avahi EntryGroupNew failed (avahi-daemon down?)";
      return nullptr;
    }
    dbus::MessageReader reader(resp.value().get());
    if (!reader.PopObjectPath(&group_path)) {
      LOG(WARNING) << "avahi EntryGroupNew: missing object path in reply";
      return nullptr;
    }
  }

  dbus::ObjectProxy* group =
      bus->GetObjectProxy(kAvahiService, group_path);

  // 2) AddService — interface, protocol, flags, name, type, domain,
  //    host, port, txt[][].
  char hostname[256] = {};
  gethostname(hostname, sizeof(hostname) - 1);
  std::string name = std::string("FydeOS VNC on ") + hostname;
  std::vector<std::string> txt = {
      "txtvers=1",
      "u=fydeos",
      std::string("model=") + model,
  };
  {
    dbus::MethodCall call(kAvahiEntryGroupIface, "AddService");
    dbus::MessageWriter w(&call);
    w.AppendInt32(kAvahiIfUnspec);     // interface
    w.AppendInt32(kAvahiProtoUnspec);  // protocol
    w.AppendUint32(0);                  // flags
    w.AppendString(name);
    w.AppendString("_rfb._tcp");
    w.AppendString("");      // domain — empty means .local
    w.AppendString("");      // host — empty means own hostname
    w.AppendUint16(static_cast<uint16_t>(port));
    AppendTxtEntries(&w, txt);
    auto resp = group->CallMethodAndBlock(&call, kAvahiTimeoutMs);
    if (!resp.has_value() || !resp.value()) {
      LOG(WARNING) << "avahi AddService failed for " << name;
      return nullptr;
    }
  }

  // 3) Commit — actually announces the service on the network.
  {
    dbus::MethodCall call(kAvahiEntryGroupIface, "Commit");
    auto resp = group->CallMethodAndBlock(&call, kAvahiTimeoutMs);
    if (!resp.has_value() || !resp.value()) {
      LOG(WARNING) << "avahi Commit failed";
      return nullptr;
    }
  }

  LOG(INFO) << "mDNS: announced " << name << " _rfb._tcp on :" << port;
  return std::unique_ptr<MdnsPublication>(new MdnsPublication(group));
}

MdnsPublication::MdnsPublication(dbus::ObjectProxy* group_proxy)
    : group_proxy_(group_proxy) {}

MdnsPublication::~MdnsPublication() {
  if (!group_proxy_) {
    return;
  }
  // Free() releases the entry group on the daemon side; the service
  // is withdrawn from the network immediately. We ignore the response
  // — best-effort cleanup during shutdown / toggle-off.
  dbus::MethodCall call(kAvahiEntryGroupIface, "Free");
  (void)group_proxy_->CallMethodAndBlock(&call, kAvahiTimeoutMs);
  LOG(INFO) << "mDNS: withdrew _rfb._tcp service";
}

}  // namespace fydeos::vnc::net
