#!/bin/bash
# T06 — capture pipeline: geometry sanity, real (non-blank) frame content,
# geometry vs ListDisplays, reconnect stability, update stream.
source "$(dirname "$0")/lib/common.sh"
banner "capture pipeline"
require_device
require_listening
rfb_endpoint || { echo "cannot reach RFB"; summary; exit 1; }

probe="$($VNCRFB --host "$RFB_HOST" --port "$RFB_PORT" probe 2>&1)"
geom="$(echo "$probe" | sed -n 's/^geometry=//p')"
w="${geom%x*}"; h="${geom#*x}"
if [[ -n "$w" && "$w" -gt 0 && "$h" -gt 0 ]]; then
  pass "server reports geometry $geom"
else
  fail "no geometry from probe: $probe"; summary; exit 1
fi
if (( w % 4 == 0 )); then
  pass "framebuffer width $w is padded to a multiple of 4"
else
  fail "framebuffer width $w not a multiple of 4"
fi

# geometry should match the display currently captured (allowing 0-3px pad)
mode="$(capture_mode)"
info "capture mode: $mode"
dw="$(list_displays | python3 -c '
import sys, json
d = json.load(sys.stdin)
cur = d.get("current")
for disp in d.get("displays", []):
    if disp.get("id") == cur or disp.get("active"):
        print(disp["width"], disp["height"]); break
' 2>/dev/null || true)"
if [[ -n "$dw" ]]; then
  read -r ew eh <<< "$dw"
  if (( w >= ew && w <= ew + 3 && h == eh )); then
    pass "framebuffer geometry matches captured display ${ew}x${eh}"
  else
    warn "framebuffer $geom vs display ${ew}x${eh} (rotation/scale?)"
  fi
else
  skip "display geometry unavailable from ListDisplays"
fi

# frame content must not be blank
shot="$ARTIFACTS/t06_frame.png"
out="$($VNCRFB --host "$RFB_HOST" --port "$RFB_PORT" screenshot "$shot" 2>&1)"
colors="$(echo "$out" | sed -n 's/^unique_colors=//p')"
if [[ -n "$colors" && "$colors" -ge 16 ]]; then
  pass "captured frame has real content ($colors unique colors) -> $shot"
elif [[ -n "$colors" ]]; then
  warn "frame nearly uniform ($colors colors) — screen off or solid color?"
else
  fail "screenshot failed: $out"
fi

# reconnect: a second, subsequent session must work (idle pause/resume path)
assert "second connect after disconnect succeeds (pause/resume)" \
  bash -c "$VNCRFB --host $RFB_HOST --port $RFB_PORT probe"

# continuous update stream for 8s; fps informative (KMS ~5, framesink up to 30)
watch="$($VNCRFB --host "$RFB_HOST" --port "$RFB_PORT" watch 8 2>&1)"
if [[ "$watch" == updates=* ]]; then
  pass "incremental update stream healthy: $watch"
else
  fail "watch stream error: $watch"
fi

tunnel_close
summary
