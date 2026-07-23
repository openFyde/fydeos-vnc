#!/bin/bash
# T01 — daemon lifecycle: upstart jobs, sandboxing, D-Bus presence, respawn.
source "$(dirname "$0")/lib/common.sh"
banner "service lifecycle"
require_device

out="$(dssh 'status fydeos-vncd' 2>&1)"
assert_contains "upstart job fydeos-vncd is running" "start/running" "$out"

out="$(dssh 'status fydeos-vnc-wsproxy' 2>&1)"
assert_contains "upstart job fydeos-vnc-wsproxy is running" "start/running" "$out"

out="$(dssh 'ps -o user= -C fydeos-vncd | head -1')"
assert_contains "daemon runs as user fydeos-vnc (sandboxed)" "fydeos-vnc" "$out"

out="$(vnc_dbus GetStatus)"
assert_contains "daemon owns org.fydeos.VncManager on D-Bus" "method return" "$out"

# state dir provisioned by tmpfiles.d
out="$(dssh 'stat -c "%a %U" /var/lib/fydeos-vncd')"
assert_contains "state dir owned by fydeos-vnc" "fydeos-vnc" "$out"
assert_contains "state dir mode 700" "700" "$out"

# respawn: kill the daemon, upstart must bring it back
oldpid="$(dssh 'pgrep -o fydeos-vncd')"
if [[ -n "$oldpid" ]]; then
  dssh "kill -9 $oldpid" || true
  sleep 4
  newpid="$(dssh 'pgrep -o fydeos-vncd' || true)"
  if [[ -n "$newpid" && "$newpid" != "$oldpid" ]]; then
    pass "daemon respawns after crash (pid $oldpid -> $newpid)"
  else
    fail "daemon did not respawn within 4s (old=$oldpid new=${newpid:-none})"
  fi
  # D-Bus name must be re-claimed
  sleep 2
  out="$(vnc_dbus GetStatus)"
  assert_contains "D-Bus name re-claimed after respawn" "method return" "$out"
else
  skip "could not determine daemon pid"
fi

summary
