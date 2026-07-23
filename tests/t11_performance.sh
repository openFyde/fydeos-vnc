#!/bin/bash
# T11 — performance sanity against the design targets:
#   idle (enabled, no viewer): < 1% CPU
#   one viewer streaming:      < 15% CPU (all cores)
# Results are reported as PASS/WARN (hardware varies); numbers land in the log.
source "$(dirname "$0")/lib/common.sh"
banner "performance sanity"
require_device
require_listening
rfb_endpoint || { echo "cannot reach RFB"; summary; exit 1; }

# CPU% of the daemon over an interval, via /proc (utime+stime delta)
cpu_sample() {  # seconds -> prints percent
  local secs="$1"
  dssh "
    pid=\$(pgrep -o fydeos-vncd); [ -z \"\$pid\" ] && echo ERR && exit
    hz=\$(getconf CLK_TCK 2>/dev/null || echo 100)
    t1=\$(awk '{print \$14+\$15}' /proc/\$pid/stat)
    sleep $secs
    t2=\$(awk '{print \$14+\$15}' /proc/\$pid/stat)
    awk -v a=\$t1 -v b=\$t2 -v hz=\$hz -v s=$secs 'BEGIN{printf \"%.1f\", (b-a)*100/hz/s}'
  "
}

# --- idle: make sure no client is connected, then sample
sleep 2
idle="$(cpu_sample 10)"
if [[ "$idle" == "ERR" ]]; then
  fail "daemon process not found"
elif awk -v v="$idle" 'BEGIN{exit !(v < 1.0)}'; then
  pass "idle CPU ${idle}% (< 1% target)"
elif awk -v v="$idle" 'BEGIN{exit !(v < 3.0)}'; then
  warn "idle CPU ${idle}% (target < 1%)"
else
  fail "idle CPU ${idle}% — far above the < 1% target"
fi

# --- streaming: one viewer pulling updates for 15s
$VNCRFB --host "$RFB_HOST" --port "$RFB_PORT" --timeout 25 watch 16 \
  > "$ARTIFACTS/t11_watch.log" 2>&1 &
WPID=$!
sleep 2
stream="$(cpu_sample 12)"
wait "$WPID" 2>/dev/null
info "stream stats: $(tail -1 "$ARTIFACTS/t11_watch.log")"
if [[ "$stream" == "ERR" ]]; then
  fail "daemon process not found during streaming"
elif awk -v v="$stream" 'BEGIN{exit !(v < 15.0)}'; then
  pass "streaming CPU ${stream}% (< 15% target)"
elif awk -v v="$stream" 'BEGIN{exit !(v < 30.0)}'; then
  warn "streaming CPU ${stream}% (target < 15%)"
else
  fail "streaming CPU ${stream}% — far above target"
fi

# --- input-path latency probe (informative only): time to first update after
# a full refresh request approximates the capture->encode->send path
t0="$(date +%s%N)"
$VNCRFB --host "$RFB_HOST" --port "$RFB_PORT" screenshot "$ARTIFACTS/t11_lat.png" >/dev/null 2>&1
t1="$(date +%s%N)"
ms=$(( (t1 - t0) / 1000000 ))
info "full-frame grab round-trip: ${ms} ms (includes TLS handshake; end-to-end input latency is manual case M-PRF-1)"

tunnel_close
summary
