// Copyright 2026 The FydeOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FYDEOS_VNC_NET_PASSWORD_H_
#define FYDEOS_VNC_NET_PASSWORD_H_

#include <optional>
#include <string>
#include <string_view>

#include <base/files/file_path.h>

namespace fydeos::vnc::net {

// Bcrypt cost factor. Cost 12 = ~250ms on a modern x86 core.
constexpr int kBcryptCost = 12;

// Compute a bcrypt hash of |plaintext|. Returns the modular crypt format
// string (e.g. "$2b$12$...") or std::nullopt on error.
std::optional<std::string> HashPassword(std::string_view plaintext);

// Verify |plaintext| against |hash|. Returns true on match. Runs in
// constant-ish time courtesy of crypt(3)'s internal compare.
bool VerifyPassword(std::string_view plaintext, std::string_view hash);

// Read a hash from |path| if it exists, trimming a trailing newline.
std::optional<std::string> LoadPasswordHash(const base::FilePath& path);

// Write |hash| atomically (write tmp + rename) to |path| with mode 0600.
bool SavePasswordHash(const base::FilePath& path, std::string_view hash);

// VeNCrypt is one-way (bcrypt) but legacy VncAuth requires the server
// to hold the password in plaintext (it encrypts a random challenge
// with the password as a DES key). When "allow insecure clients" is
// on, we mirror the password to a separate file alongside the bcrypt
// hash. Calls share the atomic-write semantics of SavePasswordHash.
std::optional<std::string> LoadPasswordPlain(const base::FilePath& path);
bool SavePasswordPlain(const base::FilePath& path,
                       std::string_view plaintext);

// Validate the 16-byte VncAuth response against |challenge| using
// |password| as the DES key (per RFB 3.8 §7.2.2; key bits are reversed
// in each byte before use). Returns true on match.
bool VerifyVncAuthResponse(std::string_view password,
                            const uint8_t challenge[16],
                            const uint8_t response[16]);

}  // namespace fydeos::vnc::net

#endif  // FYDEOS_VNC_NET_PASSWORD_H_
