# Shared helpers for the fydeos-vnc test suite. Sourced by every t*.sh script.

set -u
LIBDIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TESTDIR="$(dirname "$LIBDIR")"
source "$TESTDIR/env.sh"

VNCRFB="python3 $LIBDIR/vncrfb.py"
mkdir -p "$ARTIFACTS"

PASS_N=0; FAIL_N=0; WARN_N=0; SKIP_N=0
_TEST_NAME="$(basename "${0:-interactive}" .sh)"

pass() { PASS_N=$((PASS_N+1)); echo "  [PASS] $*"; }
fail() { FAIL_N=$((FAIL_N+1)); echo "  [FAIL] $*"; }
warn() { WARN_N=$((WARN_N+1)); echo "  [WARN] $*"; }
skip() { SKIP_N=$((SKIP_N+1)); echo "  [SKIP] $*"; }
info() { echo "  ....   $*"; }

# assert <description> <command...>  — PASS/FAIL by exit code
assert() {
  local desc="$1"; shift
  if "$@" >/dev/null 2>&1; then pass "$desc"; else fail "$desc  (cmd: $*)"; fi
}

# assert_contains <description> <needle> <haystack>
assert_contains() {
  local desc="$1" needle="$2" hay="$3"
  if [[ "$hay" == *"$needle"* ]]; then pass "$desc"
  else fail "$desc  (missing '$needle' in: ${hay:0:200})"; fi
}

assert_not_contains() {
  local desc="$1" needle="$2" hay="$3"
  if [[ "$hay" != *"$needle"* ]]; then pass "$desc"
  else fail "$desc  (unexpected '$needle' in: ${hay:0:200})"; fi
}

# SSH to the device. Uses sshpass if available and DEVICE_PASSWORD is set.
SSH_OPTS=(-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null
          -o ConnectTimeout=10 -o LogLevel=ERROR -p "$DEVICE_SSH_PORT")
[[ -n "${DEVICE_SSH_KEY:-}" ]] && SSH_OPTS+=(-o IdentitiesOnly=yes -i "$DEVICE_SSH_KEY")
dssh() {
  if command -v sshpass >/dev/null 2>&1 && [[ -n "$DEVICE_PASSWORD" ]]; then
    sshpass -p "$DEVICE_PASSWORD" ssh "${SSH_OPTS[@]}" \
      "$DEVICE_USER@$DEVICE_IP" "$@"
  else
    ssh "${SSH_OPTS[@]}" "$DEVICE_USER@$DEVICE_IP" "$@"
  fi
}

# SSH tunnels. tunnel_open <local_port> <target_host> <target_port> opens
# localhost:<local_port> -> device-side <target_host>:<target_port>.
# All tunnel PIDs are tracked; tunnel_close kills them all.
TUNNEL_PIDS=()
tunnel_open() {
  local lport="$1" thost="$2" tport="$3"
  if command -v sshpass >/dev/null 2>&1 && [[ -n "$DEVICE_PASSWORD" ]]; then
    sshpass -p "$DEVICE_PASSWORD" ssh "${SSH_OPTS[@]}" -N \
      -L "$lport:$thost:$tport" "$DEVICE_USER@$DEVICE_IP" &
  else
    ssh "${SSH_OPTS[@]}" -N -L "$lport:$thost:$tport" \
      "$DEVICE_USER@$DEVICE_IP" &
  fi
  TUNNEL_PIDS+=($!)
  for _ in $(seq 1 20); do
    (exec 3<>/dev/tcp/127.0.0.1/"$lport") 2>/dev/null && exec 3>&- && return 0
    sleep 0.5
  done
  return 1
}
tunnel_close() {
  local p
  for p in "${TUNNEL_PIDS[@]:-}"; do
    [[ -n "$p" ]] && kill "$p" 2>/dev/null && wait "$p" 2>/dev/null
  done
  TUNNEL_PIDS=()
  return 0
}

# D-Bus helpers (run on the device as root)
VNC_DEST="org.fydeos.VncManager"
VNC_PATH="/org/fydeos/VncManager"
vnc_dbus() {  # vnc_dbus Method [args...]
  local method="$1"; shift
  dssh "dbus-send --system --print-reply --dest=$VNC_DEST $VNC_PATH $VNC_DEST.$method $*" 2>&1
}

# get_status -> sets STATUS_STATE STATUS_FP STATUS_BOUND_IP
get_status() {
  local out; out="$(vnc_dbus GetStatus)"
  mapfile -t _f < <(echo "$out" | sed -n 's/^ *string "\(.*\)"$/\1/p')
  STATUS_STATE="${_f[0]:-}"; STATUS_PEER="${_f[1]:-}"
  STATUS_FP="${_f[2]:-}"; STATUS_BOUND_IP="${_f[3]:-}"
}

# Wait until GetStatus state equals $1 (timeout $2 seconds, default 15)
wait_state() {
  local want="$1" t="${2:-15}"
  for _ in $(seq 1 "$t"); do
    get_status
    [[ "$STATUS_STATE" == "$want" ]] && return 0
    sleep 1
  done
  return 1
}

vnc_enable()  { vnc_dbus SetEnabled boolean:true  >/dev/null; }
vnc_disable() { vnc_dbus SetEnabled boolean:false >/dev/null; }

# ListDisplays JSON on stdout
list_displays() { vnc_dbus ListDisplays | sed -n 's/^ *string "\(.*\)"$/\1/p' | sed 's/\\"/"/g'; }

# Current capture mode: framesink | kms | disabled
capture_mode() { list_displays | python3 -c 'import sys,json;print(json.load(sys.stdin)["mode"])' 2>/dev/null; }

# TCP reachability from the host
port_open() { timeout 5 bash -c "exec 3<>/dev/tcp/$1/$2" 2>/dev/null; }

# TCP reachability tested from ON the device (works behind NAT).
# The device's bash lacks /dev/tcp support, so use python3.
device_port_open() {  # host port
  dssh "python3 -c \"import socket;socket.create_connection(('$1',$2),5)\"" 2>/dev/null
}

# Device LAN IP: configured, else from GetStatus bound_ip
device_lan_ip() {
  if [[ -n "$DEVICE_LAN_IP" ]]; then echo "$DEVICE_LAN_IP"; return; fi
  get_status; echo "$STATUS_BOUND_IP"
}

# Establish the endpoints test clients should use, transparently handling a
# device behind NAT (only SSH forwarded):
#   RFB_HOST:RFB_PORT   -> reaches the daemon's LAN listener (VeNCrypt path)
#   WEB_HOST:WEB_PORT   -> reaches the noVNC/websockify HTTPS port
#   RFB_DIRECT=1|0      -> whether the device LAN is directly routable
rfb_endpoint() {
  if port_open "$DEVICE_IP" "$VNC_PORT"; then
    RFB_DIRECT=1
    RFB_HOST="$DEVICE_IP"; RFB_PORT="$VNC_PORT"
    WEB_HOST="$DEVICE_IP"; WEB_PORT="$NOVNC_PORT"
    return 0
  fi
  RFB_DIRECT=0
  local lan_ip; lan_ip="$(device_lan_ip)"
  if [[ -z "$lan_ip" ]]; then
    echo "ERROR: cannot determine device LAN IP for tunneling" >&2
    return 1
  fi
  tunnel_open "$TUNNEL_LAN_PORT" "$lan_ip" "$VNC_PORT" || return 1
  tunnel_open "$TUNNEL_WEB_PORT" "127.0.0.1" "$NOVNC_PORT" || true
  RFB_HOST=127.0.0.1; RFB_PORT="$TUNNEL_LAN_PORT"
  WEB_HOST=127.0.0.1; WEB_PORT="$TUNNEL_WEB_PORT"
  trap tunnel_close EXIT
  info "device not directly routable — using SSH tunnels (LAN path via $lan_ip)"
  return 0
}

summary() {
  echo
  echo "== $_TEST_NAME: PASS=$PASS_N FAIL=$FAIL_N WARN=$WARN_N SKIP=$SKIP_N =="
  # machine-readable line for run_all.sh
  echo "$_TEST_NAME PASS=$PASS_N FAIL=$FAIL_N WARN=$WARN_N SKIP=$SKIP_N" \
    >> "$ARTIFACTS/results.txt"
  [[ $FAIL_N -eq 0 ]]
}

banner() { echo; echo "=== $_TEST_NAME: $* ==="; }

require_device() {
  if ! dssh true 2>/dev/null; then
    echo "ERROR: cannot SSH to $DEVICE_USER@$DEVICE_IP:$DEVICE_SSH_PORT" >&2
    echo "$_TEST_NAME PASS=0 FAIL=0 WARN=0 SKIP=1" >> "$ARTIFACTS/results.txt"
    exit 3
  fi
}

# Ensure server is enabled and listening before protocol-level tests
require_listening() {
  get_status
  if [[ "$STATUS_STATE" != "Listening" ]]; then
    vnc_enable; wait_state Listening 20 || {
      echo "ERROR: server did not reach Listening state" >&2; exit 3; }
  fi
}
