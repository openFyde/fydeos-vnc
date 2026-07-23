# fydeos-vnc tests

End-to-end test suite for the FydeOS VNC daemon. These tests drive the
daemon through its real external interfaces (D-Bus control API, RFB/VeNCrypt
protocol, TLS, noVNC web access) against a running device, complementing the
gtest unit tests built by `BUILD.gn` (`run_fydeos_vnc_tests`).

## Layout

- `run_all.sh` — run the whole suite and print a combined report.
- `t01`–`t11` — one script per functional area (lifecycle, D-Bus API,
  network binding, TLS/cert, authentication, capture, input, multi-display,
  session transition, noVNC web, performance).
- `lib/vncrfb.py` — self-contained RFB 3.8 client (VeNCrypt X509None/X509Plain/
  Plain, SecTypeNone, VncAuth) with `probe`/`screenshot`/`sectypes`/input/
  `watch` subcommands. Standard library only.
- `lib/common.sh` — shared helpers (SSH, tunnels, D-Bus, assertions).
- `env.sh` — device address, ports, and options.
- `docs/test-document.md`, `docs/test-document.zh.md` — the full test plan,
  including the manual test cases that the scripts do not cover.

## Running

Requires a Linux host with `bash`, `ssh`, `curl`, `openssl`, `python3`
(3.8+), and `root` SSH access to a device with the VNC feature installed.
`sshpass` is recommended for password SSH; `avahi-utils` enables the mDNS
check.

```bash
export DEVICE_IP=<device-ip> DEVICE_SSH_PORT=<port> DEVICE_PASSWORD=<pw>
./run_all.sh                 # all non-destructive tests
./run_all.sh t05 t06         # a subset
FORCE=1 ./run_all.sh         # also run t09 (logs the device user out!)
```

Each check reports `[PASS]`/`[FAIL]`/`[WARN]`/`[SKIP]`; artifacts (screenshots,
logs, `results.txt`) land in `artifacts/` (git-ignored). The suite restores a
clean no-password state on exit; `t04` rotates the TLS certificate and `t09`
(opt-in) ends the user session. If the device is behind NAT with only SSH
forwarded, the suite tunnels the VNC ports automatically.

See `docs/test-document.md` for the full plan and the manual cases.
