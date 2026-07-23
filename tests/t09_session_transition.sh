#!/bin/bash
# T09 — session transition resilience: restarting the UI (equivalent to a
# logout / Chrome crash) must not kill the VNC service; capture falls back to
# the KMS path and keeps serving frames at the login screen.
#
# DESTRUCTIVE: ends the current user session on the device.
# Run explicitly with:  FORCE=1 ./t09_session_transition.sh
source "$(dirname "$0")/lib/common.sh"
banner "session transition (logout resilience)"

if [[ "${FORCE:-0}" != "1" ]]; then
  skip "destructive test (logs out the device user) — run with FORCE=1"
  summary; exit 0
fi
require_device
require_listening
rfb_endpoint || { echo "cannot reach RFB"; summary; exit 1; }

pre_mode="$(capture_mode)"
info "mode before: $pre_mode"

# background watcher: hold a client session open across the transition
$VNCRFB --host "$RFB_HOST" --port "$RFB_PORT" --timeout 40 watch 35 \
  > "$ARTIFACTS/t09_watch.log" 2>&1 &
WATCH_PID=$!
sleep 2

dssh 'restart ui' >/dev/null 2>&1 || true
sleep 3

# daemon must still be alive and listening
get_status
if [[ "$STATUS_STATE" == "Listening" ]]; then
  pass "daemon still Listening after UI restart"
else
  # brief WaitingForCrtc is acceptable while the login screen comes up
  if wait_state Listening 20; then
    pass "daemon recovered to Listening after UI restart"
  else
    fail "daemon state after UI restart: $STATUS_STATE"
  fi
fi

# capture must have fallen back to KMS (Chrome mojo pipe died)
for _ in $(seq 1 20); do
  m="$(capture_mode)"; [[ "$m" == "kms" ]] && break; sleep 1
done
if [[ "$m" == "kms" ]]; then
  pass "capture fell back to KMS after session end"
else
  warn "capture mode after UI restart: '$m' (expected kms at login screen)"
fi

# a fresh client must get real frames of the login screen
sleep 3
shot="$ARTIFACTS/t09_login.png"
out="$($VNCRFB --host "$RFB_HOST" --port "$RFB_PORT" screenshot "$shot" 2>&1)"
colors="$(echo "$out" | sed -n 's/^unique_colors=//p')"
if [[ -n "$colors" && "$colors" -ge 16 ]]; then
  pass "login screen captured post-transition ($colors colors) -> $shot"
else
  fail "no usable frame after transition: $out"
fi

# the client that was connected during the transition: session survival is
# best-effort (a reconnect is acceptable), so report it as information
wait "$WATCH_PID" 2>/dev/null
info "client held across transition reported: $(tail -1 "$ARTIFACTS/t09_watch.log")"

info "recovery to framesink after re-login is manual case M-SES-1"
tunnel_close
summary
