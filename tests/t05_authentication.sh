#!/bin/bash
# T05 — authentication matrix:
#   LAN, no password      -> VeNCrypt X509None only
#   LAN, password         -> VeNCrypt X509Plain; wrong password rejected
#   loopback, no password -> SecTypeNone
#   loopback, password    -> VeNCrypt Plain
#   insecure mode         -> VncAuth (DES) available on LAN
# Restores a clean no-password state at the end.
source "$(dirname "$0")/lib/common.sh"
banner "authentication matrix"
require_device
require_listening
rfb_endpoint || { echo "cannot reach RFB"; summary; exit 1; }

reset_auth() { vnc_dbus ClearPassword >/dev/null 2>&1; vnc_dbus ClearInsecurePassword >/dev/null 2>&1; }
cleanup() { reset_auth; tunnel_close 2>/dev/null; }
trap cleanup EXIT
reset_auth

# --- LAN, no password
sec="$($VNCRFB --host "$RFB_HOST" --port "$RFB_PORT" sectypes 2>&1)"
assert_contains "LAN advertises VeNCrypt" "19(VeNCrypt)" "$sec"
assert_not_contains "LAN does not advertise SecTypeNone" "1(None)" "$sec"
assert_not_contains "LAN does not advertise VncAuth (no insecure mode)" "2(VncAuth)" "$sec"
assert_contains "no password -> X509None subtype" "260(X509None)" "$sec"
assert "X509None full handshake succeeds" \
  bash -c "$VNCRFB --host $RFB_HOST --port $RFB_PORT probe"

# --- LAN, with password
vnc_dbus SetPassword "string:$TEST_VNC_PASSWORD" >/dev/null
sleep 1
sec="$($VNCRFB --host "$RFB_HOST" --port "$RFB_PORT" sectypes 2>&1)"
assert_contains "password set -> X509Plain subtype" "262(X509Plain)" "$sec"
assert_not_contains "password set -> X509None no longer offered" "260(X509None)" "$sec"

if $VNCRFB --host "$RFB_HOST" --port "$RFB_PORT" --password "wrong-pw" probe >/dev/null 2>&1; then
  fail "wrong password was accepted"
else
  pass "wrong password rejected"
fi
assert "correct password accepted (X509Plain)" \
  bash -c "$VNCRFB --host $RFB_HOST --port $RFB_PORT --password $TEST_VNC_PASSWORD probe"

# password changes must apply live (no daemon restart): change and retest
vnc_dbus SetPassword "string:${TEST_VNC_PASSWORD}x" >/dev/null
sleep 1
if $VNCRFB --host "$RFB_HOST" --port "$RFB_PORT" --password "$TEST_VNC_PASSWORD" probe >/dev/null 2>&1; then
  fail "old password still accepted after SetPassword change"
else
  pass "old password invalid immediately after change"
fi
assert "new password accepted immediately" \
  bash -c "$VNCRFB --host $RFB_HOST --port $RFB_PORT --password ${TEST_VNC_PASSWORD}x probe"

# --- loopback paths (through SSH tunnel to device 127.0.0.1)
if tunnel_open "$TUNNEL_PORT" 127.0.0.1 "$VNC_PORT"; then
  # with password: loopback offers VeNCrypt Plain (outer wss:// handles crypto)
  sec="$($VNCRFB --host 127.0.0.1 --port "$TUNNEL_PORT" sectypes 2>&1)"
  assert_contains "loopback+password advertises VeNCrypt" "19(VeNCrypt)" "$sec"
  assert_contains "loopback+password offers Plain subtype" "256(Plain)" "$sec"
  assert "loopback Plain auth succeeds" \
    bash -c "$VNCRFB --host 127.0.0.1 --port $TUNNEL_PORT --password ${TEST_VNC_PASSWORD}x probe"

  # without password: loopback is SecTypeNone
  vnc_dbus ClearPassword >/dev/null; sleep 1
  sec="$($VNCRFB --host 127.0.0.1 --port "$TUNNEL_PORT" sectypes 2>&1)"
  assert_contains "loopback no-password advertises SecTypeNone" "1(None)" "$sec"
  assert "loopback SecTypeNone handshake succeeds" \
    bash -c "$VNCRFB --host 127.0.0.1 --port $TUNNEL_PORT --security none probe"
else
  skip "could not open SSH tunnel — loopback auth paths not verified"
fi

# --- insecure mode: password.plain enables VncAuth on LAN
vnc_dbus SetInsecurePassword "string:$TEST_VNC_PASSWORD" >/dev/null
sleep 1
sec="$($VNCRFB --host "$RFB_HOST" --port "$RFB_PORT" sectypes 2>&1)"
if [[ "$sec" == *"2(VncAuth)"* ]]; then
  pass "insecure mode advertises VncAuth on LAN"
  assert "VncAuth DES challenge-response succeeds" \
    bash -c "$VNCRFB --host $RFB_HOST --port $RFB_PORT --security vncauth --password $TEST_VNC_PASSWORD probe"
  if $VNCRFB --host "$RFB_HOST" --port "$RFB_PORT" --security vncauth --password bad probe >/dev/null 2>&1; then
    fail "VncAuth accepted a wrong password"
  else
    pass "VncAuth rejects wrong password"
  fi
else
  fail "insecure mode did not expose VncAuth (sectypes: $sec)"
fi

# cleanup restores no-password, secure-only state (trap)
summary
