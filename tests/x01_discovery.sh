#!/bin/bash
# X01 — discovery & external reachability (run from a host on the DUT's LAN).
#
# On-device network binding (bound address class, listener teardown/restore)
# is now covered by the Tast test vnc.NetworkBinding. This script keeps the
# parts that only an external observer on the same LAN can verify: that the
# mDNS announcement actually propagates on the wire and that the RFB/noVNC
# ports are reachable from another machine.
source "$(dirname "$0")/lib/common.sh"
banner "discovery & external reachability"
require_device
require_listening



lan_ip="$(device_lan_ip)"
info "device LAN address: ${lan_ip:-unknown}"

# --- mDNS _rfb._tcp announcement, observed from the host's own network stack.
# This is the meaningful check: it proves the record is visible to other hosts
# on the LAN, which an on-device D-Bus query to avahi cannot establish.
if command -v avahi-browse >/dev/null 2>&1; then
  mdns="$(timeout 6 avahi-browse -rpt _rfb._tcp 2>/dev/null || true)"
  if echo "$mdns" | grep -qi "FydeOS"; then
    pass "mDNS _rfb._tcp 'FydeOS VNC' record visible from this host"
  else
    warn "no 'FydeOS VNC' _rfb._tcp record seen from this host — run on the DUT's LAN (not through NAT/a tunnel), and check for mDNS filtering"
  fi
else
  skip "avahi-browse not installed on this host (install avahi-utils) — mDNS propagation not verified"
fi

# --- direct reachability of the RFB and noVNC ports from this host.
# Only meaningful when the host shares the DUT's LAN; behind NAT the ports are
# not forwarded and this is expected to fail (use the tunnels for functional
# runs instead).
if [[ -n "$lan_ip" ]] && port_open "$lan_ip" "$VNC_PORT"; then
  pass "RFB port $VNC_PORT reachable at $lan_ip from this host"
else
  warn "RFB port not directly reachable at ${lan_ip}:$VNC_PORT — run on the DUT's LAN (behind NAT this is expected)"
fi
if [[ -n "$lan_ip" ]] && port_open "$lan_ip" "$NOVNC_PORT"; then
  pass "noVNC port $NOVNC_PORT reachable at $lan_ip from this host"
else
  warn "noVNC port not directly reachable at ${lan_ip}:$NOVNC_PORT"
fi

summary
