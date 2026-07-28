#!/bin/bash
# Run the external-observer shell suite and print a combined report.
#
# On-device functional coverage now lives in the Tast bundle
# (fydeos-tast-tests, tests named vnc.*). This shell suite is the complement
# for checks that need a separate host on the DUT's LAN — discovery, concurrent
# viewers, host-side performance — plus the vncrfb.py client for manual repro.
#
#   ./run_all.sh              run all checks
#   ./run_all.sh x02          run a subset
cd "$(dirname "$0")"
source ./env.sh
mkdir -p "$ARTIFACTS"
: > "$ARTIFACTS/results.txt"

# preflight
if ! python3 lib/vncrfb.py selftest >/dev/null 2>&1; then
  echo "FATAL: vncrfb.py selftest failed (python3 required)"; exit 1
fi

if [[ $# -gt 0 ]]; then
  tests=()
  for t in "$@"; do tests+=("$(ls "${t}"*.sh 2>/dev/null | head -1)"); done
else
  tests=(x01_*.sh x02_*.sh x03_*.sh x04_*.sh)
fi

overall=0
for t in "${tests[@]}"; do
  [[ -z "$t" || ! -f "$t" ]] && continue
  bash "$t" 2>&1 | tee "$ARTIFACTS/$(basename "$t" .sh).log"
  rc=${PIPESTATUS[0]}
  [[ $rc -eq 1 ]] && overall=1
done

echo
echo "==================== COMBINED REPORT ===================="
tp=0; tf=0; tw=0; ts=0
while read -r name rest; do
  p="${rest#PASS=}"; p="${p%% *}"
  f="${rest#*FAIL=}"; f="${f%% *}"
  w="${rest#*WARN=}"; w="${w%% *}"
  s="${rest#*SKIP=}"; s="${s%% *}"
  printf '  %-28s PASS=%-3s FAIL=%-3s WARN=%-3s SKIP=%s\n' "$name" "$p" "$f" "$w" "$s"
  tp=$((tp+p)); tf=$((tf+f)); tw=$((tw+w)); ts=$((ts+s))
done < "$ARTIFACTS/results.txt"
echo "  ---------------------------------------------------------"
printf '  %-28s PASS=%-3s FAIL=%-3s WARN=%-3s SKIP=%s\n' TOTAL "$tp" "$tf" "$tw" "$ts"
echo "  artifacts: $ARTIFACTS/"
echo "========================================================="
exit $overall
