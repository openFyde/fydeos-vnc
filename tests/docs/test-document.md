# FydeOS VNC — Test Document

This document describes how to test the FydeOS VNC remote-access feature. It
covers three complementary methods:

- **On-device automated tests (Tast)** — the `vnc.*` tests run on the device
  and exercise the daemon end to end: D-Bus control API, RFB/VeNCrypt protocol,
  TLS, authentication, capture, input, the Settings UI, multi-display, noVNC web
  access, session transitions, and CPU.
- **External-observer tests (shell suite)** — a small set of checks that only
  make sense from a separate host on the DUT's LAN: mDNS propagation, concurrent
  viewers, and host-side performance.
- **Manual tests** — the few cases that need a human, a real third-party
  client, or physical hardware: the OOBE flow, input feel and IME,
  real-client clipboard behavior, display experience, client
  compatibility, network change, and performance feel.

Together the `vnc.*` Tast tests, the `x0*` shell checks, and the manual cases
(`M-*`) cover all VNC functions. The coverage matrix in section 5 maps every
function to its cases.

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
| TigerVNC (or another VeNCrypt-capable viewer) | manual cases | for manual tests |

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
| `vnc.RebootPersistence` | *(host-driven)* The enabled state (Chrome pref) persists across a reboot |
| `vnc.SuspendResume` | *(host-driven)* The daemon keeps serving after a suspend/resume cycle |

### 3.2 External-observer suite (shell)

These need a separate host on the DUT's LAN; they complement, not duplicate,
the Tast tests.

```bash
cd tests
./run_all.sh            # x01 + x02 + x03
./run_all.sh x02        # a subset
```

Each check prints `[PASS]`/`[FAIL]`/`[WARN]`/`[SKIP]`; `run_all.sh` prints a
combined report under `tests/artifacts/`.

| Script | What it verifies |
|---|---|
| `x01_discovery.sh` | mDNS `_rfb._tcp` "FydeOS VNC" record is visible to other hosts on the LAN; RFB/noVNC ports reachable from another machine |
| `x02_multiclient.sh` | Several viewers connect at once — each completes a handshake and frame grab — and the daemon stays healthy afterwards |
| `x03_performance.sh` | Host-side full-frame round-trip (connect+TLS+auth+first frame) and the update throughput/fps a viewer observes over the wire |

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

These are the cases the automated `vnc.*` suite cannot fully exercise: they
need a human, a real third-party client, or physical hardware. Anything a test
can check has been automated and removed from this section — the coverage
matrix in section 5 shows where each function is covered. Record each case as
Pass / Fail / Blocked with notes. "Viewer" means a VeNCrypt-capable VNC client
(TigerVNC recommended) unless stated otherwise.

### 4.1 OOBE flow

The OOBE setup screen only appears when a VNC client is connected as OOBE
finishes, so it cannot be reached by the automated suite.

**M-OOBE-1 — Remote setup, keep enabled with password**
Precondition: device freshly reset, VNC connectable during OOBE.
1. From a viewer, walk through OOBE remotely.
2. At the end of OOBE, expected: a VNC setup screen appears (because a VNC
   session is active) explaining the feature, with password + confirm fields,
   an "allow insecure connections" option, Enable and Disable buttons.
3. Enter matching passwords and click Enable.
   Expected: setup completes; VNC stays enabled; reconnecting now requires
   the new password.

**M-OOBE-2 — Remote setup, disable at the end**
As M-OOBE-1, but click Disable. Expected: VNC service is disabled after
OOBE; no further connections possible; Settings shows it off.

**M-OOBE-3 — Enable without password**
As M-OOBE-1, but click Enable with empty password fields.
Expected: a confirmation dialog warns about enabling without a password;
cancelling returns to the form; confirming enables passwordless (TLS-only)
access.

**M-OOBE-4 — Local setup (no VNC session)**
Complete OOBE locally with no VNC client connected.
Expected: the VNC setup screen is *not* shown; after OOBE the VNC service is
disabled by default.

### 4.2 Viewing and input

**M-INP-3 — Keyboard layouts and IME**
Precondition: logged in, a non-US layout or CJK IME added on the DUT.
1. Type letters, symbols, and shifted characters into a text field via VNC.
   Expected: characters match the DUT's active layout.
2. Switch to the IME and compose text (e.g. pinyin -> candidate selection).
   Expected: composition and candidate windows behave as with a local
   keyboard.

**M-CLP-1 — Confidential clipboard is not pushed**
Precondition: logged in, a viewer connected, a password saved in the DUT's
password manager. (Clipboard transfer itself — both directions, connect-time
sync, UTF-8, and a ~1 MB payload — is automated by `vnc.Clipboard`. Only this
security check needs a human: copying a stored password requires the
device-authentication dialog, which cannot be scripted.)
1. In the password manager, copy a saved password (complete the
   authentication prompt).
   Expected: the password is NOT pushed to any connected viewer — the
   viewer's clipboard keeps its previous contents.
2. Copy ordinary text afterwards.
   Expected: it is pushed normally, proving the filter is content-specific
   and not a stuck state.

### 4.3 Clients, web access, discovery

**M-CLI-1 — Client compatibility matrix**
Connect, authenticate (with and without password), view, and type with each
client; record results per client:
- TigerVNC (Linux/Windows) — primary reference client
- RealVNC Viewer
- noVNC (any modern browser, via the built-in web access)
- A mobile client (e.g. AVNC on Android, or another VeNCrypt-capable app)
Expected: VeNCrypt-capable clients work fully. Clients without VeNCrypt/TLS
support must be *refused* unless "allow insecure connections" was enabled —
that refusal is correct behavior, not a bug.

**M-NET-2 — Network change**
While enabled, switch the DUT between Wi-Fi and Ethernet (or change APs).
Expected: the service re-binds to the new LAN address; Settings shows the
updated address; a viewer can connect on the new address. (Runtime re-bind is
a known product gap — record the actual behavior.)

### 4.4 Performance

**M-PRF-1 — Latency and quality feel**
On an idle LAN, with one viewer at native resolution:
1. Type in a terminal/editor via VNC. Expected: echo feels immediate
   (target < 80 ms; compare side-by-side with the physical screen).
2. Drag a window in circles; play a video fullscreen.
   Expected: motion is coherent (some blur/tearing acceptable); the UI stays
   responsive; DUT does not overheat or starve foreground apps.

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
| Insecure-client mode (VncAuth/plain) | `vnc.AuthMatrix` | M-CLI-1 |
| Loopback/web auth fast path | `vnc.AuthMatrix`, `vnc.NoVNCWeb` | — |
| LAN-only binding | `vnc.NetworkBinding` | M-NET-2 |
| mDNS discovery | `vnc.NetworkBinding` (present + withdrawn); `x01` (external propagation) | — |
| Pre-login (KMS) capture | `vnc.CaptureKMS`, `vnc.SessionTransition`, `vnc.LoginScreenInput` | — |
| In-session (compositor) capture | `vnc.CaptureChrome` | M-PRF-1 |
| Damage/update stream, fps | `vnc.CaptureChrome`, `vnc.Performance`; `x03` | M-PRF-1 |
| Framebuffer geometry/padding/resize | `vnc.CaptureKMS`, `vnc.MultiDisplay` | — |
| Pre-login / lock-screen input | `vnc.LockScreen`, `vnc.LoginScreenInput` | — |
| In-session input (buttons, wheel, drag, keys) | `vnc.InputInjection`, `vnc.InputParity` | M-INP-3 |
| IME / keyboard layout | — | M-INP-3 |
| Clipboard (both directions) | `vnc.Clipboard` (viewer→device paste; device→viewer legacy + Extended Clipboard, connect-time sync, UTF-8, ~1 MB payload) | — |
| Confidential clipboard filtered | — (needs the un-scriptable device-auth dialog; a Chrome-side unit test is the automated route) | M-CLP-1 |
| Multi-display enumeration/switch/content/input | `vnc.MultiDisplay` | — |
| Display hotplug | `vnc.DisplayHotplug` | — |
| Logout/login/crash resilience | `vnc.SessionTransition` | — |
| Lock screen | `vnc.LockScreen` | — |
| Suspend/resume | `vnc.SuspendResume` | — |
| Reboot persistence | `vnc.RebootPersistence` | — |
| noVNC web access | `vnc.NoVNCWeb`, `vnc.NoVNCSession`; `x01`, `x03` | — |
| OOBE setup screen | — | M-OOBE-1…4 |
| Concurrent viewers | `x02` | M-CLI-1 |
| Client compatibility | protocol level: `vnc.AuthMatrix` + two independent RFB client implementations | M-CLI-1 |
| CPU targets | `vnc.Performance` | M-PRF-1 |
| Host-side latency/throughput | `x03` | M-PRF-1 |
| Auth abuse resilience | `vnc.AuthAbuse` | — |

Notes:
- What genuinely needs a human (all that is left in section 4): the OOBE setup
  screen (M-OOBE-1…4), IME / non-US layout (M-INP-3), the confidential-clipboard check (M-CLP-1, which needs the
  device-authentication dialog),
  network change /
  runtime re-bind (M-NET-2, a product gap), third-party client compatibility
  (M-CLI-1), and the subjective latency / quality feel (M-PRF-1).
- Every other former manual case — enable/disable, fingerprint, password,
  certificate regeneration, pre-login & lock-screen input, session transitions, suspend/resume, mDNS,
  auth abuse, no-plaintext, display hotplug, full noVNC session — is fully
  automated and has been deleted from this section; see the matrix above.
- Tight encoding is exercised only by real viewers (the noVNC session and
  M-CLI-1); the automated RFB test clients decode Raw.

---

## 6. Recommended test passes

- **Smoke (automated, ≈4 min)**: `tast run <dut> vnc.Lifecycle vnc.DBusAPI
  vnc.CaptureKMS` — service up, API sane, pixels flowing.
- **Full automated (24 tests, ≈15–25 min)**: `tast run <dut> 'vnc.*'` on a
  test-image DUT (attach a second display for the multi-display / hotplug
  checks), then `./run_all.sh` from a host on the DUT's LAN for the external
  checks. Includes reboot, suspend/resume, and the login-screen flow, so it
  runs longer than the per-test times suggest.
- **Release regression**: full automated pass + all manual cases in section 4,
  including the client matrix and the OOBE cases on a freshly reset device.
