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
- **Manual tests** — cases that need a human at the screen or a real VNC
  client: Settings look-and-feel, OOBE flow, input feel, clipboard, IME, session
  transitions, and client compatibility.

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
| Performance | Idle < 1% CPU, streaming < 15% CPU, LAN input latency < 80 ms |

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

---

## 3. Automated tests

### 3.1 On-device suite (Tast) — primary functional coverage

The `vnc.*` tests run on the device and are the primary automated coverage.
Run the whole set against a test-image DUT:

```bash
tast run <dut-ip>[:ssh-port] 'vnc.*'
```

Results, per-test logs, and failure screenshots are collected by Tast in its
results directory. The tests are state-safe: each restores a clean, no-password
state, and the session-transition test restores the login screen.

| Test | What it verifies |
|---|---|
| `vnc.Lifecycle` | Both system jobs running; daemon sandboxed as the `fydeos-vnc` user; state dir permissions; D-Bus name owned; automatic respawn after a kill and D-Bus re-claim |
| `vnc.DBusAPI` | `GetStatus` fields; enable/disable round-trip closes/opens the RFB port; `StatusChanged` signal; `ListDisplays` JSON; typed error for a bogus `SetCaptureDisplay`; password files (mode 0600, bcrypt); `RegenerateCertificate` |
| `vnc.NetworkBinding` | Bound address is a real, up LAN interface (RFC-1918/link-local); RFB + noVNC listeners; teardown on disable and restore on enable; avahi mDNS announcement present when enabled and withdrawn when disabled |
| `vnc.TLSCertificate` | cert/key modes; RSA-4096 self-signed; fingerprint identical across D-Bus and the RFB wire; TLS 1.1 rejected / 1.2 accepted; regeneration is live without restart |
| `vnc.AuthMatrix` | LAN offers only VeNCrypt (X509None without password, X509Plain with); wrong password rejected; changes effective immediately; loopback SecTypeNone / VeNCrypt Plain; insecure-mode VncAuth (DES) |
| `vnc.CaptureKMS` | Pre-login KMS path: KMS mode, width padded to ×4, a decodable non-blank frame |
| `vnc.CaptureChrome` | Post-login FrameSink path: a solid-colour page is reproduced in the captured framebuffer (closed loop) |
| `vnc.InputInjection` | RFB pointer and key events injected over the wire produce the expected reaction in the Chrome session |
| `vnc.Settings` | The os-settings VNC card drives the daemon: enable toggle round-trip, fingerprint display matches D-Bus, certificate regeneration |
| `vnc.NoVNCWeb` | noVNC page served over HTTPS with the daemon certificate; plain HTTP handled per policy; websockify backend and WebSocket upgrade |
| `vnc.SessionTransition` | KMS (pre-login) → FrameSink (login) → KMS (logout) with the daemon never dropping and frames served throughout |
| `vnc.MultiDisplay` | Display inventory; switching the captured display changes the framebuffer to that display's geometry and back; unknown-id error path (switching checks skip with one display) |
| `vnc.Performance` | Daemon CPU idle (< 1% target) and while a viewer streams; reported to the perf pipeline |
| `vnc.InputParity` | Right-click opens a context menu, middle-click opens a new tab, and the wheel scrolls — all injected over RFB |
| `vnc.Clipboard` | A viewer's RFB cut-text is pasted into the focused field (device→viewer push is not wired) |
| `vnc.AuthAbuse` | Repeated wrong passwords are refused and the daemon stays healthy (no per-IP backoff is implemented) |
| `vnc.SettingsPassword` | Setting a password through the os-settings dialog makes the daemon require it |
| `vnc.LockScreen` | The daemon keeps serving the lock screen while the session is locked |
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

Record each case as Pass / Fail / Blocked with notes. "Viewer" means a
VeNCrypt-capable VNC client (TigerVNC recommended) unless stated otherwise.

### 4.1 Settings UI

**M-SET-1 — Enable/disable toggle**
Precondition: logged in.
1. Open Settings and locate the VNC / remote access section.
2. Turn the toggle on. Expected: status becomes active; connection info
   (IP:port) and the TLS fingerprint appear; a viewer can connect.
3. Turn the toggle off. Expected: active viewer is disconnected; new
   connections are refused; state persists across a reboot.

**M-SET-2 — Fingerprint verification**
1. With VNC enabled, read the SHA-256 fingerprint in Settings.
2. Connect with a viewer that displays the server certificate.
   Expected: the fingerprints match exactly.

**M-SET-3 — Password set and clear**
1. Set a password in Settings. Expected: a viewer now requires it; the wrong
   password is refused; the right one connects.
2. Clear the password. Expected: viewer connects without a password (TLS
   still on).

**M-SET-4 — Regenerate certificate**
1. Note the current fingerprint; click regenerate.
   Expected: a new fingerprint appears; a viewer that trusted the old
   certificate warns about the change; Settings warns if sessions are active.

### 4.2 OOBE flow

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

### 4.3 Viewing and input

**M-INP-1 — Pre-login control**
Precondition: device at the login/lock screen, VNC enabled.
1. Connect and verify the login screen is rendered and updates live.
2. Click into the password field, type a wrong password, press Enter.
   Expected: clicks and keystrokes land correctly (dots appear, error shows);
   pointer position matches the local screen.

**M-INP-2 — In-session pointer parity**
Precondition: logged in, viewer connected.
1. Right-click the desktop. Expected: context menu opens.
2. Middle-click a link in the browser. Expected: opens in a new tab.
3. Scroll with the wheel in a long page, both directions. Expected: smooth
   scrolling.
4. Drag a window by its title bar; drag-select text. Expected: drag works
   without sticking or dropping.

**M-INP-3 — Keyboard layouts and IME**
Precondition: logged in, a non-US layout or CJK IME added on the DUT.
1. Type letters, symbols, and shifted characters into a text field via VNC.
   Expected: characters match the DUT's active layout.
2. Switch to the IME and compose text (e.g. pinyin → candidate selection).
   Expected: composition and candidate windows behave as with a local
   keyboard.

**M-CLP-1 — Clipboard**
Precondition: viewer with clipboard support (TigerVNC / noVNC clipboard
panel).
1. Copy text on the viewer side, paste into an editor on the DUT via VNC.
2. Copy text on the DUT, check it arrives in the viewer clipboard.
   Expected: both directions transfer plain text correctly (long text up to
   ~1 MB; non-ASCII preserved with a client using the extended clipboard).

### 4.4 Displays and sessions

**M-DSP-1 — Display selection experience**
Precondition: two displays connected, logged in.
1. Using the display-selection interface (or the automated switch in
   `vnc.MultiDisplay`), view each display in turn.
   Expected: correct image on each; input lands on the display being viewed;
   geometry changes are handled by the viewer without artifacts.

**M-DSP-2 — Display hotplug**
1. While viewing the external display, unplug it.
   Expected: the session falls back to the remaining/primary display rather
   than freezing or disconnecting.
2. Re-plug the display. Expected: it reappears in the display list and can be
   selected again.

**M-SES-1 — Login/logout while viewing**
1. Connect at the login screen, log in while watching remotely.
   Expected: transition to the desktop is visible; brief quality change
   (login-screen mode is lower fps) is acceptable; no permanent freeze or
   disconnect; input works after the transition.
2. Log out while connected. Expected: view returns to the login screen and
   stays live; input works at the login screen.

**M-SES-2 — Lock screen**
Lock the session while a viewer is connected.
Expected: lock screen is shown remotely; unlocking via VNC (password typing)
works.

**M-SES-3 — Suspend / resume**
1. With a viewer connected, suspend the DUT (close lid or power menu), then
   resume it.
   Expected: the daemon reattaches after resume — reconnecting the viewer
   yields a live picture; the device did not fail to suspend because of VNC.

### 4.5 Clients, web access, discovery

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

**M-WEB-1 — Full noVNC session**
1. Browse to `https://<DUT_IP>:6080/vnc.html`, accept the self-signed
   certificate warning.
2. Connect, enter the password if set, operate the desktop (type, click,
   scroll), use the noVNC clipboard panel, then disconnect.
   Expected: full session works in the browser with no plugin.

**M-NET-1 — mDNS discovery**
On a client with zeroconf/Bonjour browsing (e.g. `avahi-browse -rt
_rfb._tcp`, or a mobile VNC app's discovery view), verify the device
announces itself ("FydeOS VNC on <hostname>") with the right port when
enabled, and the announcement disappears when disabled.

**M-NET-2 — Network change**
While enabled, switch the DUT between Wi-Fi and Ethernet (or change APs).
Expected: the service re-binds to the new LAN address; Settings shows the
updated address; a viewer can connect on the new address.

### 4.6 Security and performance

**M-SEC-1 — Failed-authentication behavior**
With a password set, attempt several consecutive wrong-password logins, then
the correct one.
Expected: wrong attempts are always refused; the service stays up
(no crash/DoS); note whether rapid retries are throttled.

**M-SEC-2 — No plaintext on the wire**
With default settings (insecure mode off), attempt to connect with a client
configured for unencrypted VNC only.
Expected: connection is refused before any screen data is sent.

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
| Settings toggle & status | `vnc.DBusAPI`, `vnc.Settings` | M-SET-1 |
| Enable/disable listener lifecycle | `vnc.DBusAPI`, `vnc.NetworkBinding` | M-SET-1 |
| TLS mandatory, cert, fingerprint | `vnc.TLSCertificate`, `vnc.AuthMatrix` | M-SET-2, M-SEC-2 |
| Certificate regeneration | `vnc.DBusAPI`, `vnc.TLSCertificate`, `vnc.Settings` | M-SET-4 |
| Password auth (bcrypt, live update) | `vnc.DBusAPI`, `vnc.AuthMatrix`, `vnc.SettingsPassword` | — |
| Insecure-client mode (VncAuth/plain) | `vnc.AuthMatrix` | M-CLI-1 |
| Loopback/web auth fast path | `vnc.AuthMatrix`, `vnc.NoVNCWeb` | M-WEB-1 |
| LAN-only binding | `vnc.NetworkBinding` | M-NET-2 |
| mDNS discovery | `vnc.NetworkBinding` (present + withdrawn); `x01` (external propagation) | — |
| Pre-login (KMS) capture | `vnc.CaptureKMS`, `vnc.SessionTransition` | M-INP-1 |
| In-session (compositor) capture | `vnc.CaptureChrome` | M-PRF-1 |
| Damage/update stream, fps | `vnc.CaptureChrome`, `vnc.Performance`; `x03` | M-PRF-1 |
| Framebuffer geometry/padding/resize | `vnc.CaptureKMS`, `vnc.MultiDisplay` | M-DSP-1 |
| Pre-login input (virtual devices) | — | M-INP-1 |
| In-session input (buttons, wheel, keys) | `vnc.InputInjection`, `vnc.InputParity` | M-INP-2 (drag), M-INP-3 |
| IME / keyboard layout | — | M-INP-3 |
| Clipboard (viewer→device paste) | `vnc.Clipboard` | M-CLP-1 (device→viewer push, not implemented) |
| Multi-display enumeration/switch | `vnc.MultiDisplay` | M-DSP-1 |
| Display hotplug | `vnc.DisplayHotplug` | — |
| Logout/login/crash resilience | `vnc.SessionTransition` | M-SES-1 |
| Lock screen | `vnc.LockScreen` | M-SES-2 (unlock via VNC keystrokes) |
| Suspend/resume | `vnc.SuspendResume` | — |
| Reboot persistence | `vnc.RebootPersistence` | — |
| noVNC web access | `vnc.NoVNCWeb`, `vnc.NoVNCSession`; `x01`, `x03` | — |
| OOBE setup screen | — | M-OOBE-1…4 |
| Concurrent viewers | `x02` | M-CLI-1 |
| Client compatibility | protocol level: `vnc.AuthMatrix`, `vnc.CaptureChrome` | M-CLI-1 |
| CPU targets | `vnc.Performance` | M-PRF-1 |
| Host-side latency/throughput | `x03` | M-PRF-1 |
| Auth abuse resilience | `vnc.AuthAbuse` | — |

Notes:
- What stays manual: pre-login input verification (M-INP-1), IME/layout
  (M-INP-3), pointer drag (part of M-INP-2), the device→viewer clipboard
  direction (M-CLP-1, not implemented in the daemon), unlocking specifically
  via VNC keystrokes (M-SES-2), network change (M-NET-2), client compatibility
  with third-party viewers (M-CLI-1), the
  subjective latency/quality feel (M-PRF-1), and the OOBE setup screen
  (M-OOBE-1…4). The OOBE screen only appears when a VNC client is connected as
  OOBE finishes, so it is exercised through M-OOBE, not the automated suite.

---

## 6. Recommended test passes

- **Smoke (automated, ≈4 min)**: `tast run <dut> 'vnc.Lifecycle vnc.DBusAPI
  vnc.CaptureKMS'` — service up, API sane, pixels flowing.
- **Full automated (≈10 min)**: `tast run <dut> 'vnc.*'` on a test-image DUT
  (attach a second display for the multi-display switching checks), then
  `./run_all.sh` from a host on the DUT's LAN for the external checks.
- **Release regression**: full automated pass + all manual cases in section 4,
  including the client matrix and the OOBE cases on a freshly reset device.
