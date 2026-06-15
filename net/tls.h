// Copyright 2026 The FydeOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FYDEOS_VNC_NET_TLS_H_
#define FYDEOS_VNC_NET_TLS_H_

#include <string>

#include <base/files/file_path.h>
#include <openssl/ssl.h>

namespace fydeos::vnc::net {

// Ensures a self-signed RSA-4096 X.509 cert + key pair exists at the given
// paths. If both files already exist they are reused as-is; otherwise a new
// pair is generated with:
//   - subject CN = device hostname (gethostname)
//   - SAN: DNS:<hostname>
//   - 10-year validity (per design §5.5.3)
//   - cert mode 0644, key mode 0600
// Returns true on success.
bool EnsureSelfSignedCertificate(const base::FilePath& cert_path,
                                 const base::FilePath& key_path);

// Computes the SHA-256 fingerprint of the DER-encoded cert at |cert_path|,
// formatted as colon-separated uppercase hex (e.g.
// "AB:CD:EF:..."). Returns the empty string on error.
std::string GetCertificateFingerprint(const base::FilePath& cert_path);

// Build a process-wide SSL_CTX for the server. Loads the PEM cert/key
// pair, sets TLS 1.2 as the minimum protocol, and disables session
// caching (each VeNCrypt connection is short-lived and ephemeral).
// Returns nullptr on error.
SSL_CTX* MakeServerSSLContext(const base::FilePath& cert_path,
                              const base::FilePath& key_path);

}  // namespace fydeos::vnc::net

#endif  // FYDEOS_VNC_NET_TLS_H_
