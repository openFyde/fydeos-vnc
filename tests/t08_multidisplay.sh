#!/bin/bash
# T08 — multi-display: ListDisplays inventory, SetCaptureDisplay switching with
# geometry verification, switch-back to primary, error paths.
# Requires an active user session (framesink mode); KMS mode is primary-only.
source "$(dirname "$0")/lib/common.sh"
banner "multi-display"
require_device
require_listening
rfb_endpoint || { echo "cannot reach RFB"; summary; exit 1; }

mode="$(capture_mode)"
if [[ "$mode" != "framesink" ]]; then
  # KMS mode: verify the documented error instead
  out="$(vnc_dbus SetCaptureDisplay int64:1 2>&1 || true)"
  if [[ "$out" == *"Error.KmsMode"* || "$out" == *"Error.NotRunning"* ]]; then
    pass "SetCaptureDisplay correctly refused in $mode mode ($(echo "$out" | grep -o 'Error\.[A-Za-z]*' | head -1))"
  else
    warn "unexpected response in $mode mode: ${out:0:150}"
  fi
  skip "framesink mode required for switching tests — log in and re-run"
  summary; exit 0
fi

json="$(list_displays)"
echo "$json" > "$ARTIFACTS/t08_displays.json"
count="$(echo "$json" | python3 -c 'import sys,json;print(len(json.load(sys.stdin)["displays"]))')"
info "$count display(s): $(echo "$json" | python3 -c '
import sys, json
out = []
for d in json.load(sys.stdin)["displays"]:
    star = "*" if d.get("isPrimary") else ""
    out.append(str(d["id"]) + ":" + str(d["width"]) + "x" + str(d["height"]) + star)
print(", ".join(out))')"

switch_and_check() {  # id expected_w expected_h
  local id="$1" ew="$2" eh="$3"
  vnc_dbus SetCaptureDisplay "int64:$id" >/dev/null 2>&1
  for _ in $(seq 1 10); do
    sleep 1
    geom="$($VNCRFB --host "$RFB_HOST" --port "$RFB_PORT" probe 2>/dev/null | sed -n 's/^geometry=//p')"
    local w="${geom%x*}" h="${geom#*x}"
    if [[ -n "$w" ]] && (( w >= ew && w <= ew + 3 && h == eh )); then
      return 0
    fi
  done
  echo "$geom"; return 1
}

while read -r id ew eh; do
  [[ -z "$id" ]] && continue
  if got="$(switch_and_check "$id" "$ew" "$eh")"; then
    pass "SetCaptureDisplay($id) -> framebuffer ${ew}x${eh}"
  else
    fail "SetCaptureDisplay($id): framebuffer '$got', expected ${ew}x${eh}"
  fi
done < <(echo "$json" | python3 -c '
import sys, json
for d in json.load(sys.stdin)["displays"]:
    print(d["id"], d["width"], d["height"])')

# switch back to primary via the documented 0 = primary shorthand
if vnc_dbus SetCaptureDisplay int64:0 >/dev/null 2>&1; then
  pass "SetCaptureDisplay(0) returns to primary"
else
  fail "SetCaptureDisplay(0) failed"
fi

# error path — wait for framesink mode to settle first (display switches
# rebuild the capture stream and pass through KMS briefly)
for _ in $(seq 1 15); do
  [[ "$(capture_mode)" == "framesink" ]] && break
  sleep 1
done
out="$(vnc_dbus SetCaptureDisplay int64:424242424242 2>&1 || true)"
if [[ "$out" == *"Error.UnknownDisplay"* ]]; then
  pass "unknown display id -> Error.UnknownDisplay"
elif [[ "$out" == *"Error.KmsMode"* ]]; then
  warn "capture still in KMS when testing unknown id (Error.KmsMode) — switch recovery slow"
else
  fail "unknown display id: unexpected response: ${out:0:150}"
fi

if [[ "$count" -lt 2 ]]; then
  info "only one display connected — hotplug scenarios in manual case M-DSP-2"
fi

tunnel_close
summary
