# FydeOS VNC — Test Document

This document describes how to test the FydeOS VNC remote-access feature. It
covers two complementary automated suites:

- **On-device automated tests (Tast)** — the `vnc.*` tests run on the device
  and exercise the daemon end to end: D-Bus control API, RFB/VeNCrypt protocol,
  TLS, authentication, capture, input, the Settings UI, multi-display, noVNC web
  access, session transitions, and CPU.
- **External-observer tests (shell suite)** — a small set of checks that only
  make sense from a separate host on the DUT's LAN: mDNS propagation, concurrent
  viewers, and host-side performance.
Together the `vnc.*` Tast tests and the `x0*` shell checks cover all VNC
functions — there are no manual test cases. The coverage matrix in section 5
maps every function to the test that covers it.

---

## 1. Feature overview (what is being tested)

FydeOS VNC lets any standards-compliant VNC client on the same LAN view and
control a FydeOS device, with no internet dependency. Functions under test:

| Area | Function |
|---|---|
| Lifecycle | Service starts at boot, stays resident, toggled via Settings; auto-restarts after a crash |
| Control | D-Bus API `org.fydeos.VncManager` (enable/disable, status, password, certificate, display selection) |
| Capture | Login screen / OOBE / lock screen (KMS path) and in-session desktop (compositor path), automatic switch-over between them |
| Input | Keyboard, pointer, right/middle click, scroll wheel; pre-login and in-session paths |
| Encoding | Tight/Raw RFB encodings, damage-driven updates, framebuffer resize |
| Security | Mandatory TLS (VeNCrypt) on LAN, self-signed cert + SHA-256 fingerprint, optional password (bcrypt), optional legacy-compatible "insecure client" mode, loopback fast path for the built-in web client |
| Network | LAN-only binding, port 5900 (RFB) and 6080 (web), mDNS `_rfb._tcp` announcement |
| Web access | Built-in noVNC client served over HTTPS on port 6080 |
| Multi-display | Enumerate displays, choose which display a session views |
| Resilience | Logout / UI crash / login transitions without service loss; suspend/resume |
| Performance | Idle < 1% CPU, streaming < 15% CPU, LAN input latency < 80 ms (design targets) |

---

## 2. Test environment

### 2.1 Device under test (DUT)

- A FydeOS device with the VNC feature installed, reachable over the LAN.
- The on-device Tast tests require a **test image** (one that carries the Tast
  test runner and the `chrome` test support). The external shell checks work
  against any image with the VNC feature and `root` SSH.
- SSH access as `root`.
- For full coverage: a user account to log in with, and a second display for
  the multi-display cases.

### 2.2 Test host

A Linux machine (with the ChromeOS SDK / `tast` command for the on-device
suite) on the same LAN as the DUT:

| Tool | Needed for | Required? |
|---|---|---|
| `tast` (ChromeOS SDK) | on-device `vnc.*` suite | for the Tast tests |
| `bash`, `ssh` | shell suite | yes |
| `python3` (3.8+) | RFB protocol client | yes |
| `avahi-browse` (package `avahi-utils`) | mDNS propagation check | optional |
| `sshpass` | non-interactive SSH with password | optional |
| TigerVNC (`tigervnc-viewer`) + `xvfb` | real-client check (`x04`) | for `x04` |

No third-party Python packages are needed — the bundled RFB client
(`lib/vncrfb.py`) is standard-library only.

### 2.3 Configuration

For the shell suite, edit `tests/env.sh` (or export the variables) to point at
your DUT:

```bash
DEVICE_IP=…            # DUT LAN address
DEVICE_SSH_PORT=…      # SSH port
DEVICE_USER=root
DEVICE_PASSWORD=…      # or set DEVICE_SSH_KEY=/path/to/key
VNC_PORT=5900
NOVNC_PORT=6080
```

> Note: if the DUT sits behind NAT / port forwarding (only SSH reachable),
> `x02`/`x03` automatically tunnel the VNC ports over SSH; `x01`'s discovery
> and reachability checks need a real LAN path and warn otherwise.

---

## 3. Automated tests

### 3.1 On-device suite (Tast) — primary functional coverage

The `vnc.*` tests run on the device and are the primary automated coverage.
Run the whole set against a test-image DUT:

```bash
tast run <dut-ip>[:ssh-port] 'vnc.*'
```

Results, per-test logs, and failure screenshots are collected by Tast in its
results directory. Passwords and the enabled state are restored by each test,
and the session-transition test restores the login screen; note that
`vnc.LoginScreenInput` leaves a local account behind (cleared by the next
Chrome-based test) and the two host-driven tests reboot / suspend the device.

| Test | What it verifies |
|---|---|
| `vnc.Sanity` | The image carries the VNC feature: upstart jobs and the daemon binary present |
| `vnc.Lifecycle` | Both system jobs running; daemon sandboxed as the `fydeos-vnc` user; state dir permissions; D-Bus name owned; automatic respawn after a kill and D-Bus re-claim |
| `vnc.DBusAPI` | `GetStatus` fields; enable/disable round-trip closes/opens the RFB port; `StatusChanged` signal; `ListDisplays` JSON; typed error for a bogus `SetCaptureDisplay`; password files (mode 0600, bcrypt); `RegenerateCertificate` |
| `vnc.NetworkBinding` | Bound address is a real, up LAN interface (RFC-1918/link-local); RFB + noVNC listeners; teardown on disable and restore on enable; avahi mDNS announcement present when enabled and withdrawn when disabled |
| `vnc.TLSCertificate` | cert/key file permissions; RSA-4096 self-signed; fingerprint identical across D-Bus and the RFB wire; TLS 1.1 rejected / 1.2 accepted; regeneration is live without restart |
| `vnc.AuthMatrix` | LAN offers only VeNCrypt (X509None without password, X509Plain with); wrong password rejected; changes effective immediately; loopback SecTypeNone / VeNCrypt Plain; insecure-mode VncAuth (DES) |
| `vnc.CaptureKMS` | Pre-login KMS path: KMS mode, width padded to ×4, a decodable non-blank frame |
| `vnc.CaptureChrome` | Post-login FrameSink path: a solid-colour page is reproduced in the captured framebuffer (closed loop) |
| `vnc.InputInjection` | RFB pointer and key events injected over the wire produce the expected reaction in the Chrome session |
| `vnc.Settings` | The os-settings VNC card drives the daemon: enable toggle round-trip, fingerprint display matches D-Bus, certificate regeneration |
| `vnc.NoVNCWeb` | noVNC page served over HTTPS with the daemon certificate; websockify backend; the plain-HTTP-policy and WebSocket-upgrade checks are informational (logged, not enforced) |
| `vnc.SessionTransition` | KMS (pre-login) → FrameSink (login) → KMS (logout) with the daemon never dropping and frames served throughout |
| `vnc.MultiDisplay` | Display inventory; switching the captured display changes the framebuffer to that display's geometry and back; the frame shows that display's content and an injected click lands on it; unknown-id error path (switching checks skip with one display) |
| `vnc.Performance` | Daemon CPU while idle and while a viewer streams, reported to the perf pipeline with generous regression ceilings (the < 1% / 15% design targets are informational: the Raw-only test client is not comparable to a Tight viewer) |
| `vnc.InputParity` | Right-click opens a context menu, middle-click opens a new tab, the wheel scrolls, a drag selects text, and a caption drag moves the window — all injected over RFB |
| `vnc.Clipboard` | A viewer's RFB cut-text is pasted into the focused field; a device-side copy is pushed to viewers — legacy Latin-1 ServerCutText and Extended Clipboard (notify→request→provide, UTF-8 intact, sync on connect, ~1 MB payload) |
| `vnc.AuthAbuse` | Repeated wrong passwords are refused and the daemon stays healthy (no per-IP backoff is implemented) |
| `vnc.SettingsPassword` | Setting a password through the os-settings dialog makes the daemon require it |
| `vnc.LockScreen` | While locked the daemon serves the lock screen, and VNC-injected keystrokes drive it: wrong password rejected, correct password unlocks |
| `vnc.LoginScreenInput` | At the genuine boot login screen (no session), VNC drives the "Add Person → Use local account" flow to create a local account and sign in |
| `vnc.NoVNCSession` | The built-in noVNC web client connects and renders the desktop in a browser tab |
| `vnc.DisplayHotplug` | Forcing an external display's connector off/on updates the daemon's inventory and capture falls back gracefully (needs an external display) |
| `vnc.OOBESetup` | The OOBE setup screen appears only when a viewer is connected as onboarding finishes; Enable keeps the service listening, Disable turns it off, and a local setup with no viewer silently disables it |
| `vnc.RebootPersistence` | *(host-driven)* The enabled state (Chrome pref) persists across a reboot |
| `vnc.SuspendResume` | *(host-driven)* The daemon keeps serving after a suspend/resume cycle |

### 3.2 External-observer suite (shell)

These need a separate host on the DUT's LAN; they complement, not duplicate,
the Tast tests.

```bash
cd tests
./run_all.sh            # x01 + x02 + x03 + x04
./run_all.sh x02        # a subset
```

Each check prints `[PASS]`/`[FAIL]`/`[WARN]`/`[SKIP]`; `run_all.sh` prints a
combined report under `tests/artifacts/`.

| Script | What it verifies |
|---|---|
| `x01_discovery.sh` | mDNS `_rfb._tcp` "FydeOS VNC" record is visible to other hosts on the LAN; RFB/noVNC ports reachable from another machine |
| `x02_multiclient.sh` | Several viewers connect at once — each completes a handshake and frame grab — and the daemon stays healthy afterwards |
| `x03_performance.sh` | Host-side full-frame round-trip (connect+TLS+auth+first frame) and the update throughput/fps a viewer observes over the wire |
| `x04_tigervnc.sh` | An independent client (TigerVNC, headless under Xvfb) completes the VeNCrypt/X509 handshake, opens the real desktop, and leaves the daemon healthy |

`x01`'s discovery and reachability checks need a real LAN path; behind NAT they
warn. `x02`/`x03` tunnel the VNC ports automatically when the device is not
directly routable (tunnelled numbers include SSH overhead).

### 3.3 The RFB test client

`lib/vncrfb.py` is useful interactively when reproducing bugs:

```bash
python3 lib/vncrfb.py --host <DUT_IP> sectypes                  # advertised auth
python3 lib/vncrfb.py --host <DUT_IP> probe                     # geometry, TLS fp
python3 lib/vncrfb.py --host <DUT_IP> screenshot out.png        # grab a frame
python3 lib/vncrfb.py --host <DUT_IP> --password X click 500 300
python3 lib/vncrfb.py --host <DUT_IP> watch 10                  # fps/bandwidth
```

---

## 4. Manual test cases

None. Every VNC function is covered by the automated suites in section 3 — the
coverage matrix in section 5 maps each function to the test that covers it.

What is deliberately left to user acceptance testing rather than a scripted
case: the subjective picture-quality feel (motion, video, sharpness), and
compatibility with VNC clients other than TigerVNC and the built-in noVNC
(RealVNC, mobile apps). Also not tested: IME composition and non-US keyboard
layouts, and runtime re-binding after a network change — the daemon binds once
at start-up, so a viewer must reconnect on the new address after a
Wi-Fi/Ethernet switch.

---

## 5. Coverage matrix

| Function | Automated | Manual |
|---|---|---|
| Service start/stop/respawn/sandbox | `vnc.Lifecycle` | — |
| Settings toggle & status | `vnc.Settings`, `vnc.DBusAPI` | — |
| Enable/disable listener lifecycle | `vnc.DBusAPI`, `vnc.NetworkBinding` | — |
| Enabled state persists across reboot | `vnc.RebootPersistence` | — |
| TLS mandatory, cert, fingerprint | `vnc.TLSCertificate`, `vnc.AuthMatrix`, `vnc.Settings` | — |
| Certificate regeneration | `vnc.DBusAPI`, `vnc.TLSCertificate`, `vnc.Settings` | — |
| Password auth (bcrypt, live update) | `vnc.DBusAPI`, `vnc.AuthMatrix`, `vnc.SettingsPassword` | — |
| Insecure-client mode (VncAuth/plain) | `vnc.AuthMatrix` | — |
| Loopback/web auth fast path | `vnc.AuthMatrix`, `vnc.NoVNCWeb` | — |
| LAN-only binding | `vnc.NetworkBinding` | — |
| mDNS discovery | `vnc.NetworkBinding` (present + withdrawn); `x01` (external propagation) | — |
| Pre-login (KMS) capture | `vnc.CaptureKMS`, `vnc.SessionTransition`, `vnc.LoginScreenInput` | — |
| In-session (compositor) capture | `vnc.CaptureChrome` | — |
| Damage/update stream, fps | `vnc.CaptureChrome`, `vnc.Performance`; `x03` | — |
| Framebuffer geometry/padding/resize | `vnc.CaptureKMS`, `vnc.MultiDisplay` | — |
| Pre-login / lock-screen input | `vnc.LockScreen`, `vnc.LoginScreenInput` | — |
| In-session input (buttons, wheel, drag, keys) | `vnc.InputInjection`, `vnc.InputParity` | — |
| Clipboard (both directions) | `vnc.Clipboard` (viewer→device paste; device→viewer legacy + Extended Clipboard, connect-time sync, UTF-8, ~1 MB payload) | — |
| Multi-display enumeration/switch/content/input | `vnc.MultiDisplay` | — |
| Display hotplug | `vnc.DisplayHotplug` | — |
| Logout/login/crash resilience | `vnc.SessionTransition` | — |
| Lock screen | `vnc.LockScreen` | — |
| Suspend/resume | `vnc.SuspendResume` | — |
| Reboot persistence | `vnc.RebootPersistence` | — |
| noVNC web access | `vnc.NoVNCWeb`, `vnc.NoVNCSession`; `x01`, `x03` | — |
| OOBE setup screen (gating, Enable, Disable) | `vnc.OOBESetup` | — |
| Concurrent viewers | `x02` | — |
| Client compatibility | `x04` (TigerVNC: VeNCrypt X509 + real desktop); protocol level: `vnc.AuthMatrix` | — |
| CPU targets | `vnc.Performance` | — |
| Input (click-to-pixel) latency | `vnc.Performance` (reported, 80 ms target) | — |
| Host-side latency/throughput | `x03` | — |
| Auth abuse resilience | `vnc.AuthAbuse` | — |

Notes:
- Section 4 is empty: every function has an automated test. Left to user
  acceptance testing: subjective picture quality, and clients other than
  TigerVNC (covered by `x04`) and the built-in noVNC.
- Deliberately not tested: IME composition and non-US keyboard layouts; and
  runtime re-binding after a network change, which the daemon does not
  implement (it binds once at start-up, so a viewer must reconnect on the new
  address after a Wi-Fi/Ethernet switch).
- Every case this document once listed as manual — enable/disable,
  fingerprint, password, certificate regeneration, pre-login & lock-screen
  input, session transitions, suspend/resume, mDNS, auth abuse, no-plaintext,
  display hotplug, full noVNC session, pointer drag, multi-display
  content/input, clipboard (both directions), the OOBE setup screen, click
  latency, and TigerVNC compatibility — is now automated; see the matrix.
- Tight encoding is requested only by real viewers (the noVNC session and
  `x04`'s TigerVNC); the suite's own RFB clients decode Raw.

---

## 6. Recommended test passes

- **Smoke (automated, ≈4 min)**: `tast run <dut> vnc.Lifecycle vnc.DBusAPI
  vnc.CaptureKMS` — service up, API sane, pixels flowing.
- **Full automated (25 tests, ≈15–25 min)**: `tast run <dut> 'vnc.*'` on a
  test-image DUT (attach a second display for the multi-display / hotplug
  checks), then `./run_all.sh` from a host on the DUT's LAN for the external
  checks. Includes reboot, suspend/resume, and the login-screen flow, so it
  runs longer than the per-test times suggest.
- **Release regression**: the full automated pass above, then a UAT session
  for what a script cannot judge — picture quality in real use, and any
  additional VNC clients the release must support.
