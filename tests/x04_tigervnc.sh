#!/bin/bash
# X04 — real-client interoperability with TigerVNC (run from a host on the
# DUT's LAN).
#
# The automated suite's own RFB clients speak Raw encoding and are written
# against this daemon, so they cannot prove that an independent, widely used
# viewer interoperates. TigerVNC is that independent implementation: it
# negotiates VeNCrypt X509 and asks for Tight encoding, neither of which the
# suite's clients exercise. It runs headless here under Xvfb — no physical
# display is needed.
#
# Other clients (RealVNC, mobile apps, browsers other than the built-in noVNC)
# stay with user acceptance testing.
source "$(dirname "$0")/lib/common.sh"
banner "TigerVNC real-client interoperability"
require_device
require_listening

if ! command -v vncviewer >/dev/null 2>&1; then
  skip "vncviewer not installed on this host (install tigervnc-viewer)"
  summary; exit 0
fi
if ! command -v xvfb-run >/dev/null 2>&1; then
  skip "xvfb-run not installed on this host (install xvfb) — TigerVNC needs a display"
  summary; exit 0
fi
info "$(vncviewer --version 2>&1 | head -1)"

rfb_endpoint || { fail "cannot reach the daemon's RFB port"; summary; exit 1; }

# TigerVNC verifies the server certificate and, on any doubt, opens a modal
# approval dialog — which nothing can dismiss on a headless host. Avoid it:
#   * trust the daemon's own self-signed certificate by passing it as the CA;
#   * address the server by the name in the certificate (the daemon issues it
#     for the device hostname), so the hostname check also passes.
ca="$ARTIFACTS/device-cert.pem"
if dssh "cat /var/lib/fydeos-vncd/cert.pem" > "$ca" 2>/dev/null && [[ -s "$ca" ]]; then
  cert_cn="$(openssl x509 -in "$ca" -noout -subject 2>/dev/null | sed 's/.*CN *= *//')"
  info "daemon certificate CN: ${cert_cn:-unknown}"
else
  ca=""
  warn "could not fetch the daemon certificate — TigerVNC may block on its approval dialog"
fi

host="$RFB_HOST"
if [[ "$RFB_DIRECT" != "1" && "$RFB_HOST" == "127.0.0.1" && "$cert_cn" == "localhost" ]]; then
  # The tunnel terminates locally, so "localhost" reaches the same socket and
  # matches the certificate name.
  host="localhost"
elif [[ "$RFB_DIRECT" == "1" && -n "$cert_cn" && "$cert_cn" != "$RFB_HOST" ]]; then
  info "note: connecting by address ($RFB_HOST) while the certificate names '$cert_cn' — a real viewer warns about the hostname here"
fi
target="$host:$RFB_PORT"
info "connecting TigerVNC to $target"

# Run TigerVNC headless for a few seconds and keep its protocol log. It exits
# on its own when the X server goes away at the end of the timeout.
#   SecurityTypes  restrict to the VeNCrypt X509 flavours the daemon offers
#   X509CA         trust the daemon's own self-signed certificate
#   PreferredEncoding=Tight  ask for the encoding real viewers use
run_viewer() {  # extra args... -> writes $log
  log="$ARTIFACTS/tigervnc.$$.log"
  # -extension GLX: some hosts ship an Xvfb that crashes initialising GLX
  # against the system EGL driver; the viewer does not need it.
  timeout 40 xvfb-run -a -s "-screen 0 1280x1024x24 -extension GLX" vncviewer \
      -SecurityTypes=X509None,X509Plain \
      -X509CA="$ca" \
      -PreferredEncoding=Tight \
      -AlertOnFatalError=0 -ReconnectOnError=0 \
      -Log '*:stderr:100' \
      "$@" "$target" >"$log" 2>&1 &
  vpid=$!
  sleep 6   # handshake + first frame

  # --- Tight-path streaming CPU -------------------------------------------
  # vnc.Performance measures streaming CPU with a Raw-only client; TigerVNC is
  # the Tight-capable one, so the Tight-side number is measured here. Damage
  # is generated through a second VNC connection sweeping the pointer — the
  # daemon composites the cursor into the framebuffer, so cursor motion is
  # real damage TigerVNC must stream. NB: this is a continuous-small-damage
  # profile, not the full-screen video the <15% design target describes.
  python3 - "$RFB_HOST" "$RFB_PORT" 9 "$(dirname "$0")/lib" <<'PYEOF' >/dev/null 2>&1 &
import math, sys, time
sys.path.insert(0, sys.argv[4])
import vncrfb
c = vncrfb.RfbClient(sys.argv[1], int(sys.argv[2]), security='vencrypt')
c.connect()
end = time.time() + float(sys.argv[3])
i = 0
while time.time() < end:
    x = int(c.width/2 + (c.width/3)*math.sin(i/17.0))
    y = int(c.height/2 + (c.height/3)*math.cos(i/23.0))
    c.pointer_event(x, y, 0)
    i += 1
    time.sleep(0.02)
PYEOF
  sweep_pid=$!
  sleep 2   # let the sweep start producing damage
  # Sample the daemon's CPU on the DUT over one SSH call (CLK_TCK=100; 5s
  # window → %CPU = delta_ticks / 5).
  ticks="$(dssh 'pid=$(pgrep -o -x fydeos-vncd); r1=$(awk "{print \$14+\$15}" /proc/$pid/stat); sleep 5; r2=$(awk "{print \$14+\$15}" /proc/$pid/stat); echo $((r2-r1))' 2>/dev/null | tr -dc 0-9)"
  wait "$sweep_pid" 2>/dev/null
  if [[ -n "$ticks" ]]; then
    tight_cpu=$(( ticks / 5 ))
    info "Tight-path streaming CPU under pointer-driven damage: ~${tight_cpu}% of one core"
    if (( tight_cpu > 50 )); then
      fail "Tight streaming CPU ${tight_cpu}% is unreasonably high for cursor-sized damage"
    elif (( tight_cpu > 15 )); then
      warn "Tight streaming CPU ${tight_cpu}% exceeds the 15% design target even for cursor-sized damage"
    else
      pass "Tight-path streaming stayed cheap (~${tight_cpu}% CPU under continuous damage)"
    fi
  else
    warn "could not sample the daemon CPU over SSH — Tight streaming cost not measured"
  fi
  # ------------------------------------------------------------------------

  kill "$vpid" 2>/dev/null
  wait "$vpid" 2>/dev/null
}

run_viewer
if grep -qiE "TLS handshake completed" "$log"; then
  pass "TigerVNC completed the VeNCrypt/X509 TLS handshake"
else
  fail "TigerVNC did not complete the TLS handshake — see $log"
  info "$(tail -5 "$log" | tr '\n' ' ')"
fi

if grep -qi "hostname mismatch\|certificate.*not trusted\|unknown certificate" "$log"; then
  warn "TigerVNC objected to the certificate (it blocks on an approval dialog when headless) — see $log"
fi

# A real session: the viewer reports the desktop it was given.
if geom="$(grep -oiE "[0-9]{3,5}x[0-9]{3,5}" "$log" | head -1)" && [[ -n "$geom" ]]; then
  pass "TigerVNC opened the remote desktop ($geom)"
else
  fail "TigerVNC never received a desktop — see $log"
fi

# The viewer asked for Tight (the encoding real clients prefer, which this
# suite's own Raw-only clients never request). Neither side logs the encoding
# actually chosen, so this is not asserted directly — but a broken Tight
# rectangle would surface below as a decode error.
info "requested Tight encoding; neither viewer nor daemon logs the chosen encoding, so it is not asserted here"

if grep -qiE "protocol error|Unknown encoding|decode error" "$log"; then
  fail "TigerVNC reported a protocol/decode error — see $log"
fi

# The daemon must survive a real client connecting and disconnecting.
sleep 2
if get_status && [[ "$STATUS_STATE" == "Listening" ]]; then
  pass "daemon still Listening after the TigerVNC session"
else
  fail "daemon state is '${STATUS_STATE:-unknown}' after the TigerVNC session"
fi

summary
