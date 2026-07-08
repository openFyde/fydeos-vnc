// Copyright 2026 The FydeOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FYDEOS_VNC_DAEMON_STATE_PATHS_H_
#define FYDEOS_VNC_DAEMON_STATE_PATHS_H_

#include <base/files/file_path.h>

namespace fydeos::vnc::daemon {

// Well-known file names under the daemon state directory (--state-dir,
// default /var/lib/fydeos-vncd). Shared by main.cc, Pipeline and
// VncManagerService so every component resolves the same files.

inline base::FilePath CertPath(const base::FilePath& state_dir) {
  return state_dir.Append("cert.pem");
}

inline base::FilePath KeyPath(const base::FilePath& state_dir) {
  return state_dir.Append("key.pem");
}

inline base::FilePath PasswordPath(const base::FilePath& state_dir) {
  return state_dir.Append("password.bcrypt");
}

// Presence implies "allow insecure clients" mode — the daemon mirrors the
// plaintext password here for VncAuth challenge-response.
inline base::FilePath PasswordPlainPath(const base::FilePath& state_dir) {
  return state_dir.Append("password.plain");
}

}  // namespace fydeos::vnc::daemon

#endif  // FYDEOS_VNC_DAEMON_STATE_PATHS_H_
