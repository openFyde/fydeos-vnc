#!/bin/bash
# T07 — input injection: pre-login (uinput devices) and post-login (Ash path,
# verified by visible screen reaction to injected input).
source "$(dirname "$0")/lib/common.sh"
banner "input injection"
require_device
require_listening
rfb_endpoint || { echo "cannot reach RFB"; summary; exit 1; }

mode="$(capture_mode)"
info "capture mode: $mode"

probe="$($VNCRFB --host "$RFB_HOST" --port "$RFB_PORT" probe 2>&1)"
geom="$(echo "$probe" | sed -n 's/^geometry=//p')"
w="${geom%x*}"; h="${geom#*x}"
cx=$((w / 2)); cy=$((h / 2))

# basic protocol acceptance: server must not drop the session on input
assert "pointer move accepted" \
  bash -c "$VNCRFB --host $RFB_HOST --port $RFB_PORT pointer $cx $cy"
assert "left click accepted" \
  bash -c "$VNCRFB --host $RFB_HOST --port $RFB_PORT click $cx $cy 1"
assert "right click accepted" \
  bash -c "$VNCRFB --host $RFB_HOST --port $RFB_PORT click $cx $cy 3"
assert "wheel scroll accepted" \
  bash -c "$VNCRFB --host $RFB_HOST --port $RFB_PORT wheel $cx $cy down 2"
assert "key event accepted" \
  bash -c "$VNCRFB --host $RFB_HOST --port $RFB_PORT key Escape press"
assert "legacy cut-text accepted" \
  bash -c "$VNCRFB --host $RFB_HOST --port $RFB_PORT cuttext hello"

if [[ "$mode" == "kms" ]]; then
  # pre-login path: the uinput virtual devices must exist on the device
  devs="$(dssh 'cat /proc/bus/input/devices' 2>/dev/null)"
  assert_contains "uinput keyboard device present (pre-login sink)" \
    "fydeos-vnc keyboard" "$devs"
  assert_contains "uinput pointer device present (pre-login sink)" \
    "fydeos-vnc pointer" "$devs"
  info "visual reaction check is limited pre-login; see manual case M-INP-1"
elif [[ "$mode" == "framesink" ]]; then
  # post-login path: injected Search key toggles the launcher -> screen changes
  a="$ARTIFACTS/t07_before.png"; b="$ARTIFACTS/t07_after.png"
  $VNCRFB --host "$RFB_HOST" --port "$RFB_PORT" screenshot "$a" >/dev/null 2>&1
  $VNCRFB --host "$RFB_HOST" --port "$RFB_PORT" key Search press >/dev/null 2>&1
  sleep 2
  $VNCRFB --host "$RFB_HOST" --port "$RFB_PORT" screenshot "$b" >/dev/null 2>&1
  # close the launcher again regardless of outcome
  $VNCRFB --host "$RFB_HOST" --port "$RFB_PORT" key Escape press >/dev/null 2>&1
  if [[ ! -s "$a" || ! -s "$b" ]]; then
    fail "screenshot grab failed — cannot verify input visually"
  elif ! cmp -s "$a" "$b"; then
    pass "injected Search key produced visible screen change (Ash input path live)"
  else
    warn "no visible change after Search key — verify manually (M-INP-2)"
  fi
else
  skip "capture mode '$mode' — cannot exercise input paths"
fi

tunnel_close
summary
