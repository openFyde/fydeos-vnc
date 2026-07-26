# fydeos-vnc tests

Test material for the FydeOS VNC daemon. Functional coverage is split in two:

- **On-device automated tests — the Tast suite.** The `vnc.*` tests in the
  `fydeos-tast-tests` bundle run on the device and cover the daemon's behaviour
  end to end: lifecycle, D-Bus API, network binding, TLS/cert, the auth matrix,
  KMS and FrameSink capture, input injection, the os-settings UI, noVNC web,
  session transitions, multi-display switching, and daemon CPU. Run them with
  `tast run <dut> 'vnc.*'`. See that repo's README.
- **External-observer shell suite — this directory.** A small set of checks
  that only make sense from a *separate host on the DUT's LAN*, plus the
  `vncrfb.py` RFB client for manual reproduction. This is the complement to the
  Tast suite, not a replacement.

## Layout

- `run_all.sh` — run the external-observer checks and print a combined report.
- `x01_discovery.sh` — mDNS `_rfb._tcp` propagation and RFB/noVNC reachability
  observed from another host on the LAN.
- `x02_multiclient.sh` — several viewers connected at once; concurrent-session
  handling and post-burst health.
- `x03_performance.sh` — host-side full-frame round-trip and update throughput
  over the real transport.
- `lib/vncrfb.py` — self-contained RFB 3.8 client (VeNCrypt X509None/X509Plain/
  Plain, SecTypeNone, VncAuth) with `probe`/`screenshot`/`sectypes`/input/
  `watch` subcommands. Standard library only. Also the RFB client the Tast
  suite's Go port (`vncrfb`) is based on.
- `lib/common.sh` — shared helpers (SSH, tunnels, D-Bus, assertions).
- `env.sh` — device address, ports, and options.
- `docs/test-document.md`, `docs/test-document.zh.md` — the full test plan:
  the Tast/shell split, the coverage matrix, and the manual cases.

## Running

Requires a Linux host with `bash`, `ssh`, `python3` (3.8+), and `root` SSH
access to a device with the VNC feature installed. `avahi-utils` enables the
mDNS check; `sshpass` covers password SSH. For representative results run on a
host that shares the DUT's LAN — the discovery and performance numbers are
about the real network path.

```bash
export DEVICE_IP=<device-ip> DEVICE_SSH_PORT=<port>
export DEVICE_PASSWORD=<pw>            # or: DEVICE_SSH_KEY=/path/to/testing_rsa
./run_all.sh                            # all external checks
./run_all.sh x02                        # a subset
```

Each check reports `[PASS]`/`[FAIL]`/`[WARN]`/`[SKIP]`; artifacts (screenshots,
logs, `results.txt`) land in `artifacts/` (git-ignored). If the device is
behind NAT with only SSH forwarded, `x02`/`x03` tunnel the VNC ports
automatically; `x01`'s discovery/reachability checks need a real LAN path and
will warn otherwise.

See `docs/test-document.md` for the full plan, the Tast↔shell↔manual coverage
matrix, and the manual cases.
