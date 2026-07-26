# fydeos-vnc test suite — environment configuration.
# Override any variable in your shell, or edit this file, before running.

# Target device
DEVICE_IP="${DEVICE_IP:-}"                 # DUT address (SSH-reachable)
DEVICE_SSH_PORT="${DEVICE_SSH_PORT:-22}"
DEVICE_USER="${DEVICE_USER:-root}"
# Password for SSH (used via sshpass when installed; otherwise set up an SSH
# key or expect interactive prompts).
DEVICE_PASSWORD="${DEVICE_PASSWORD:-}"
# Optional SSH private key. Test images accept the standard testing key
# (chromite/ssh_keys/testing_rsa); copy it somewhere 0600 and point here.
DEVICE_SSH_KEY="${DEVICE_SSH_KEY:-}"

# RFB / web ports as configured on the device
VNC_PORT="${VNC_PORT:-5900}"
NOVNC_PORT="${NOVNC_PORT:-6080}"

# VNC password used by the auth tests (set and cleared by the tests themselves)
TEST_VNC_PASSWORD="${TEST_VNC_PASSWORD:-fydetest1}"

# Local ports used for SSH tunnels when the device's VNC ports are not
# directly reachable (e.g. the device sits behind NAT / a port forward).
TUNNEL_PORT="${TUNNEL_PORT:-15900}"          # -> device 127.0.0.1:VNC_PORT (loopback path)
TUNNEL_LAN_PORT="${TUNNEL_LAN_PORT:-15901}"  # -> device <LAN_IP>:VNC_PORT (LAN path)
TUNNEL_WEB_PORT="${TUNNEL_WEB_PORT:-15902}"  # -> device 127.0.0.1:NOVNC_PORT

# Device LAN IP. Leave empty to auto-detect from the daemon's GetStatus.
DEVICE_LAN_IP="${DEVICE_LAN_IP:-}"

# Where test artifacts (screenshots, logs, report) are written
ARTIFACTS="${ARTIFACTS:-./artifacts}"

if [[ -z "$DEVICE_IP" ]]; then
  echo "env.sh: set DEVICE_IP (and DEVICE_SSH_PORT/DEVICE_PASSWORD) to your test device" >&2
fi
