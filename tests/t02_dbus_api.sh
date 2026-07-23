#!/bin/bash
# T02 — D-Bus API surface: GetStatus fields, enable/disable round-trip,
# ListDisplays JSON, SetCaptureDisplay error handling, password file plumbing,
# RegenerateCertificate return value.
source "$(dirname "$0")/lib/common.sh"
banner "D-Bus API"
require_device

# --- GetStatus shape
get_status
case "$STATUS_STATE" in
  Disabled|WaitingForCrtc|Listening) pass "GetStatus state is valid: $STATUS_STATE" ;;
  *) fail "GetStatus state unexpected: '$STATUS_STATE'" ;;
esac
if [[ "$STATUS_FP" =~ ^([0-9A-F]{2}:){31}[0-9A-F]{2}$ ]]; then
  pass "GetStatus fingerprint is colon-separated SHA-256"
else
  warn "GetStatus fingerprint format: '$STATUS_FP' (empty until first enable)"
fi

# --- enable/disable round-trip
vnc_disable
if wait_state Disabled 10; then pass "SetEnabled(false) -> Disabled"; else fail "state after disable: $STATUS_STATE"; fi
if port_open "$DEVICE_IP" "$VNC_PORT"; then
  fail "port $VNC_PORT still open while Disabled"
else
  pass "port $VNC_PORT closed while Disabled"
fi

vnc_enable
if wait_state Listening 20; then
  pass "SetEnabled(true) -> Listening"
else
  # No LAN interface or panel off keeps it in WaitingForCrtc — legitimate
  warn "state after enable: $STATUS_STATE (expected Listening; check panel/network)"
fi
get_status
[[ -n "$STATUS_BOUND_IP" ]] && pass "GetStatus reports bound_ip=$STATUS_BOUND_IP" \
                            || warn "GetStatus bound_ip empty"

# --- ListDisplays JSON
json="$(list_displays)"
if echo "$json" | python3 -c 'import sys,json;d=json.load(sys.stdin);assert "mode" in d and "displays" in d' 2>/dev/null; then
  pass "ListDisplays returns valid JSON with mode+displays"
  info "mode=$(capture_mode)"
else
  fail "ListDisplays JSON invalid: ${json:0:200}"
fi

# --- SetCaptureDisplay error path (bogus id must return a typed D-Bus error)
out="$(vnc_dbus SetCaptureDisplay int64:999999999 || true)"
if [[ "$out" == *"org.fydeos.VncManager.Error."* ]]; then
  pass "SetCaptureDisplay(bogus) returns typed error ($(echo "$out" | grep -o 'Error\.[A-Za-z]*' | head -1))"
elif [[ "$out" == *"boolean false"* ]]; then
  pass "SetCaptureDisplay(bogus) returns false"
else
  fail "SetCaptureDisplay(bogus) unexpected: ${out:0:200}"
fi

# --- password plumbing (files owned by daemon, correct modes)
vnc_dbus SetPassword "string:$TEST_VNC_PASSWORD" >/dev/null
out="$(dssh 'stat -c "%a %U" /var/lib/fydeos-vncd/password.bcrypt' 2>&1)"
assert_contains "SetPassword creates password.bcrypt mode 600" "600" "$out"
out="$(dssh 'head -c4 /var/lib/fydeos-vncd/password.bcrypt')"
assert_contains "password hash is bcrypt (\$2b\$)" '$2b$' "$out"

vnc_dbus SetInsecurePassword "string:$TEST_VNC_PASSWORD" >/dev/null
assert "SetInsecurePassword creates password.plain" \
  dssh 'test -f /var/lib/fydeos-vncd/password.plain'
vnc_dbus ClearInsecurePassword >/dev/null
assert "ClearInsecurePassword removes password.plain" \
  dssh 'test ! -f /var/lib/fydeos-vncd/password.plain'

vnc_dbus ClearPassword >/dev/null
assert "ClearPassword removes password.bcrypt" \
  dssh 'test ! -f /var/lib/fydeos-vncd/password.bcrypt'

# --- RegenerateCertificate returns a fingerprint (full behavior in T04)
out="$(vnc_dbus RegenerateCertificate)"
fp="$(echo "$out" | sed -n 's/^ *string "\(.*\)"$/\1/p')"
if [[ "$fp" =~ ^([0-9A-F]{2}:){31}[0-9A-F]{2}$ ]]; then
  pass "RegenerateCertificate returns SHA-256 fingerprint"
else
  fail "RegenerateCertificate returned: '${fp:0:100}'"
fi

summary
