#!/bin/bash
# Run the whole automated fydeos-vnc test suite and print a combined report.
#
#   ./run_all.sh              run all non-destructive tests
#   FORCE=1 ./run_all.sh      also run t09 (logs out the device session!)
#   ./run_all.sh t05 t06      run a subset
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
  tests=(t01_*.sh t02_*.sh t03_*.sh t04_*.sh t05_*.sh t06_*.sh t07_*.sh \
         t08_*.sh t09_*.sh t10_*.sh t11_*.sh)
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
