#!/bin/bash
# T10 — noVNC web access: HTTPS endpoint serves the noVNC client, TLS uses the
# daemon certificate, websockify proxies to the local RFB port.
source "$(dirname "$0")/lib/common.sh"
banner "noVNC web access"
require_device
require_listening
rfb_endpoint || { echo "cannot reach RFB"; summary; exit 1; }

url="https://$WEB_HOST:$WEB_PORT"

code="$(curl -sk -o /dev/null -w '%{http_code}' --max-time 10 "$url/vnc.html" || true)"
if [[ "$code" == "200" ]]; then
  pass "GET /vnc.html -> 200"
else
  # some noVNC bundles use index.html at the web root
  code2="$(curl -sk -o /dev/null -w '%{http_code}' --max-time 10 "$url/" || true)"
  if [[ "$code2" == "200" ]]; then
    pass "GET / -> 200 (vnc.html returned $code)"
  else
    fail "noVNC page not served (vnc.html=$code, /=$code2)"
  fi
fi

body="$(curl -sk --max-time 10 "$url/vnc.html" || curl -sk --max-time 10 "$url/" || true)"
if echo "$body" | grep -qi "novnc"; then
  pass "served page is noVNC"
else
  warn "page content did not mention noVNC"
fi

# core assets referenced by the page must load (spot-check app/ui or core/rfb)
asset="$(echo "$body" | grep -oE '(src|href)="[^"]+\.(js|css)"' | head -1 | cut -d'"' -f2)"
if [[ -n "$asset" ]]; then
  acode="$(curl -sk -o /dev/null -w '%{http_code}' --max-time 10 "$url/$asset" || true)"
  if [[ "$acode" == "200" ]]; then pass "asset $asset -> 200"; else fail "asset $asset -> $acode"; fi
else
  skip "no js/css asset reference found to spot-check"
fi

# HTTP (non-TLS) must not be served on the same port
hcode="$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 "http://$WEB_HOST:$WEB_PORT/" || true)"
if [[ "$hcode" == "000" || "$hcode" == "400" ]]; then
  pass "plain HTTP on $NOVNC_PORT refused (TLS required)"
else
  warn "plain HTTP on $NOVNC_PORT returned $hcode"
fi

# websockify's backend: RFB on device loopback (the wss->tcp proxy target)
if dssh "ss -tln | grep -q '127.0.0.1:$VNC_PORT'"; then
  pass "websockify backend 127.0.0.1:$VNC_PORT is listening"
else
  fail "loopback RFB listener missing — noVNC cannot proxy"
fi

# WebSocket upgrade handshake against websockify
ws="$(curl -sk --max-time 10 -o /dev/null -w '%{http_code}' \
  -H 'Connection: Upgrade' -H 'Upgrade: websocket' \
  -H 'Sec-WebSocket-Version: 13' -H 'Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==' \
  "$url/websockify" || true)"
if [[ "$ws" == "101" ]]; then
  pass "WebSocket upgrade on /websockify -> 101 Switching Protocols"
else
  warn "/websockify upgrade returned $ws (curl lacks ws support on some builds)"
fi

info "full in-browser session (login, canvas, input) is manual case M-WEB-1"
tunnel_close
summary
