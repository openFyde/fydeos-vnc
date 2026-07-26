#!/bin/bash
# X02 — concurrent viewers (external).
#
# The Tast suite exercises the daemon with a single on-device client. This
# script connects several viewers at once from the host to confirm the daemon
# serves concurrent RFB sessions (each completing a full handshake and frame
# grab) and stays healthy afterwards. Run it from a host that can reach the
# device's RFB port (directly on the LAN, or via the suite's auto tunnel).
source "$(dirname "$0")/lib/common.sh"
banner "concurrent viewers"
require_device
require_listening
rfb_endpoint || { echo "cannot reach RFB"; summary; exit 1; }

N="${MULTICLIENT_N:-3}"
info "opening $N concurrent viewers against $RFB_HOST:$RFB_PORT"

pids=(); rc_files=()
for i in $(seq 1 "$N"); do
  rc="$ARTIFACTS/x02_client_${i}.png"
  rc_files+=("$rc")
  ( $VNCRFB --host "$RFB_HOST" --port "$RFB_PORT" --timeout 25 \
      screenshot "$rc" >/dev/null 2>&1 ) &
  pids+=($!)
done

ok=0
for i in "${!pids[@]}"; do
  if wait "${pids[$i]}" 2>/dev/null && [[ -s "${rc_files[$i]}" ]]; then
    ok=$((ok+1))
  fi
done

if [[ "$ok" -eq "$N" ]]; then
  pass "all $N concurrent viewers completed a handshake and frame grab"
else
  fail "only $ok/$N concurrent viewers succeeded"
fi

# The daemon must still accept a fresh connection after the burst.
if $VNCRFB --host "$RFB_HOST" --port "$RFB_PORT" probe >/dev/null 2>&1; then
  pass "daemon healthy after concurrent viewers (fresh connect works)"
else
  fail "daemon did not accept a connection after the concurrent burst"
fi

tunnel_close
summary
