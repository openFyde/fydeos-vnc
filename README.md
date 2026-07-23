# fydeos-vnc

LAN-only VNC server daemon for FydeOS. See the design document at
`develope/task/3/fydeos-vnc-design.html` for architecture, requirements, and
open decisions.

Layout (M0 scaffold):

```
fydeos-vnc/
├── daemon/    Entry point and D-Bus service object
├── capture/   CaptureSource abstraction + KMS/FrameSink backends
├── input/     InputSink abstraction + uinput/Ash backends
├── encoder/   Tile + Tight/JPEG encoder, libvncserver glue
├── net/       TLS, interface enumeration, mDNS (avahi)
├── mojo/      Mojo interface definitions (daemon side)
├── seccomp/   minijail seccomp policy
├── init/      Upstart job
└── tests/     End-to-end test suite (see tests/README.md)
```

Unit tests are built by `BUILD.gn` (`run_fydeos_vnc_tests`). The end-to-end
suite in `tests/` drives a running device over D-Bus and the RFB protocol; see
`tests/README.md`.

Built and installed by `chromeos-base/fydeos-vnc` (live ebuild in the FydeOS
overlay).
