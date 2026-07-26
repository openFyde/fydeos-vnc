#!/bin/bash
# X03 — host-side performance (external).
#
# Daemon-side CPU (idle < 1%, streaming) is now measured on-device by the Tast
# test vnc.Performance. This script keeps the view only a host can give: the
# round-trip to a first frame over the real network/transport, and the update
# throughput a viewer actually observes. Run from a host on the DUT's LAN for
# representative numbers (a tunnel adds SSH overhead).
source "$(dirname "$0")/lib/common.sh"
banner "host-side performance"
require_device
require_listening
rfb_endpoint || { echo "cannot reach RFB"; summary; exit 1; }

# --- Full-frame round-trip: connect + TLS + auth + first frame, from the host.
t0="$(date +%s%N)"
if $VNCRFB --host "$RFB_HOST" --port "$RFB_PORT" screenshot "$ARTIFACTS/x03_frame.png" >/dev/null 2>&1; then
  t1="$(date +%s%N)"
  ms=$(( (t1 - t0) / 1000000 ))
  pass "full-frame round-trip ${ms} ms (connect+TLS+auth+first frame)"
  info "end-to-end interactive input latency remains manual case M-PRF-1"
else
  fail "could not grab a frame from the host"
fi

# --- Update throughput a viewer observes over the wire.
watch_out="$ARTIFACTS/x03_watch.log"
if $VNCRFB --host "$RFB_HOST" --port "$RFB_PORT" --timeout 25 watch 10 >"$watch_out" 2>&1; then
  stats="$(tail -1 "$watch_out")"
  if [[ "$stats" == updates=* ]]; then
    pass "update stream healthy over the wire: $stats"
  else
    warn "watch produced no update summary: $stats"
  fi
else
  fail "watch stream failed: $(tail -1 "$watch_out")"
fi

tunnel_close
summary
