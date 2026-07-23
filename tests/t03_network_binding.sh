#!/bin/bash
# T03 — network binding & discovery: listeners on RFB/noVNC ports, LAN-only
# bind address policy, teardown on disable, mDNS announcement.
source "$(dirname "$0")/lib/common.sh"
banner "network binding & discovery"
require_device
require_listening

listeners="$(dssh "ss -tln 2>/dev/null || netstat -tln" )"
assert_contains "RFB port $VNC_PORT listening on device" ":$VNC_PORT" "$listeners"
assert_contains "loopback listener on 127.0.0.1:$VNC_PORT (for websockify)" \
  "127.0.0.1:$VNC_PORT" "$listeners"
assert_contains "noVNC websockify listening on :$NOVNC_PORT" ":$NOVNC_PORT" "$listeners"

# bound LAN address must be private/link-local (RFC1918 / 169.254)
get_status
if [[ "$STATUS_BOUND_IP" =~ ^(10\.|172\.(1[6-9]|2[0-9]|3[01])\.|192\.168\.|169\.254\.) ]]; then
  pass "bound address $STATUS_BOUND_IP is LAN-class (RFC-1918/link-local)"
elif [[ -z "$STATUS_BOUND_IP" ]]; then
  warn "no bound_ip reported (no LAN interface?)"
else
  fail "bound address $STATUS_BOUND_IP is not LAN-class"
fi

# reachability — tested from the device itself (valid behind NAT too)
lan_ip="$(device_lan_ip)"
assert "RFB port reachable on LAN address $lan_ip" \
  device_port_open "$lan_ip" "$VNC_PORT"
assert "noVNC port reachable on LAN address" \
  device_port_open "$lan_ip" "$NOVNC_PORT"
if port_open "$DEVICE_IP" "$VNC_PORT"; then
  pass "RFB port also directly reachable from test host"
else
  info "RFB port not directly reachable from host (NAT/port-forward setup) — clients on the device's own LAN should verify M-CLI-1"
fi

# disable tears the listener down, re-enable brings it back
vnc_disable; wait_state Disabled 10 >/dev/null
sleep 1
if device_port_open "$lan_ip" "$VNC_PORT"; then
  fail "RFB port still open after disable"
else
  pass "RFB listener torn down on disable"
fi
vnc_enable; wait_state Listening 20 >/dev/null
sleep 1
assert "RFB listener restored on re-enable" device_port_open "$lan_ip" "$VNC_PORT"

# mDNS announcement (needs avahi-browse on the test host or the device)
if command -v avahi-browse >/dev/null 2>&1; then
  mdns="$(timeout 6 avahi-browse -rpt _rfb._tcp 2>/dev/null || true)"
  if echo "$mdns" | grep -q "FydeOS.*VNC.*_rfb._tcp\|_rfb._tcp.*FydeOS"; then
    pass "mDNS _rfb._tcp announcement visible from test host"
  else
    warn "no 'FydeOS VNC' _rfb._tcp record seen from host (VLAN/mDNS filtering?)"
  fi
elif dssh 'command -v avahi-browse' >/dev/null 2>&1; then
  mdns="$(dssh 'timeout 6 avahi-browse -rpt _rfb._tcp 2>/dev/null' || true)"
  if echo "$mdns" | grep -q "FydeOS"; then
    pass "mDNS _rfb._tcp announcement registered (checked on device)"
  else
    warn "no _rfb._tcp record found via device avahi-browse"
  fi
else
  skip "avahi-browse not available on host or device — mDNS not verified"
fi

summary
