#!/bin/bash
# T04 — TLS & certificate: state files/permissions, fingerprint consistency
# (D-Bus vs on-disk cert vs what the network really serves), TLS 1.2 minimum,
# certificate regeneration.
source "$(dirname "$0")/lib/common.sh"
banner "TLS & certificate"
require_device
require_listening
rfb_endpoint || { echo "cannot reach RFB"; summary; exit 1; }

# --- state files and permissions
out="$(dssh 'stat -c "%n %a %U" /var/lib/fydeos-vncd/cert.pem /var/lib/fydeos-vncd/key.pem')"
assert_contains "cert.pem exists (mode 644)" "cert.pem 644" "$out"
assert_contains "key.pem exists (mode 600)" "key.pem 600" "$out"
assert_contains "key owned by fydeos-vnc" "fydeos-vnc" "$out"

# key strength / validity as designed: RSA 4096, ~10y, CN=hostname
certtext="$(dssh 'openssl x509 -in /var/lib/fydeos-vncd/cert.pem -noout -text' 2>/dev/null)"
assert_contains "certificate key is RSA 4096-bit" "4096 bit" "$certtext"
hostname="$(dssh hostname)"
cn="$(echo "$certtext" | grep -o 'Subject:.*' | head -1)"
if [[ "$cn" == *"CN = $hostname"* || "$cn" == *"CN=$hostname"* ]]; then
  pass "certificate CN matches device hostname ($hostname)"
else
  warn "certificate CN vs hostname '$hostname': $cn (cert may predate a hostname change)"
fi

# --- fingerprint consistency: D-Bus == on-disk == served on the wire
get_status
disk_fp="$(dssh 'openssl x509 -in /var/lib/fydeos-vncd/cert.pem -noout -fingerprint -sha256' | cut -d= -f2)"
if [[ "$STATUS_FP" == "$disk_fp" ]]; then
  pass "GetStatus fingerprint matches on-disk cert"
else
  fail "fingerprint mismatch: dbus=$STATUS_FP disk=$disk_fp"
fi

wire_fp="$($VNCRFB --host "$RFB_HOST" --port "$RFB_PORT" fingerprint 2>/dev/null || true)"
if [[ "$wire_fp" == "$STATUS_FP" ]]; then
  pass "fingerprint served inside VeNCrypt TLS matches GetStatus"
else
  fail "wire fingerprint '$wire_fp' != dbus '$STATUS_FP'"
fi

# websockify on the noVNC port must serve the same certificate
ws_fp="$(echo | timeout 10 openssl s_client -connect "$WEB_HOST:$WEB_PORT" 2>/dev/null \
  | openssl x509 -noout -fingerprint -sha256 2>/dev/null | cut -d= -f2 || true)"
if [[ -n "$ws_fp" && "$ws_fp" == "$STATUS_FP" ]]; then
  pass "noVNC (websockify) serves the same certificate"
else
  warn "noVNC cert fingerprint '$ws_fp' (expected '$STATUS_FP')"
fi

# --- TLS version floor: 1.1 must fail, 1.2 must succeed (inside VeNCrypt)
if $VNCRFB --host "$RFB_HOST" --port "$RFB_PORT" --tls-max 1.1 probe >/dev/null 2>&1; then
  fail "TLS 1.1 handshake was accepted (must require >= 1.2)"
else
  pass "TLS 1.1 rejected"
fi
assert "TLS 1.2 accepted" \
  bash -c "$VNCRFB --host $RFB_HOST --port $RFB_PORT --tls-max 1.2 probe"

# --- certificate regeneration rotates the key and refreshes the wire
old_fp="$STATUS_FP"
new_fp="$(vnc_dbus RegenerateCertificate | sed -n 's/^ *string "\(.*\)"$/\1/p')"
if [[ -n "$new_fp" && "$new_fp" != "$old_fp" ]]; then
  pass "RegenerateCertificate produced a new fingerprint"
else
  fail "fingerprint did not change on regenerate (old=$old_fp new=$new_fp)"
fi
sleep 2
wire_fp2="$($VNCRFB --host "$RFB_HOST" --port "$RFB_PORT" fingerprint 2>/dev/null || true)"
if [[ "$wire_fp2" == "$new_fp" ]]; then
  pass "new certificate live on the wire without daemon restart"
else
  warn "wire serves '$wire_fp2' after regenerate (expected '$new_fp') — may need reconnect/restart"
fi
# websockify only reloads the cert on its own restart
info "note: websockify picks up the new cert after 'restart fydeos-vnc-wsproxy' or reboot"

tunnel_close
summary
