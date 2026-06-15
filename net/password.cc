// Copyright 2026 The FydeOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "fydeos-vnc/net/password.h"

#include <crypt.h>
#include <fcntl.h>
#include <openssl/des.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstring>
#include <string>

#include <base/files/file_path.h>
#include <base/files/file_util.h>
#include <base/files/important_file_writer.h>
#include <base/logging.h>
#include <base/strings/string_util.h>

namespace fydeos::vnc::net {

namespace {

constexpr char kBcryptPrefix[] = "$2b$";

}  // namespace

std::optional<std::string> HashPassword(std::string_view plaintext) {
  // Empty passwords are rejected — Settings UI must treat them as "no
  // password set" rather than storing a hash of "".
  if (plaintext.empty()) {
    return std::nullopt;
  }
  char salt_buf[CRYPT_GENSALT_OUTPUT_SIZE];
  const char* salt = crypt_gensalt_rn(kBcryptPrefix, kBcryptCost,
                                       nullptr, 0,
                                       salt_buf, sizeof(salt_buf));
  if (!salt) {
    PLOG(ERROR) << "crypt_gensalt_rn failed";
    return std::nullopt;
  }
  crypt_data data{};
  // |plaintext| is a string_view; crypt_r needs a C string. Make sure it's
  // NUL-terminated by copying.
  const std::string pw(plaintext);
  const char* result = crypt_r(pw.c_str(), salt, &data);
  if (!result || result[0] == '*') {
    PLOG(ERROR) << "crypt_r failed";
    return std::nullopt;
  }
  return std::string(result);
}

bool VerifyPassword(std::string_view plaintext, std::string_view hash) {
  if (plaintext.empty() || hash.empty()) {
    return false;
  }
  crypt_data data{};
  const std::string pw(plaintext);
  const std::string h(hash);
  const char* result = crypt_r(pw.c_str(), h.c_str(), &data);
  if (!result || result[0] == '*') {
    return false;
  }
  // crypt(3) returns the full hash on match. Compare in a length-aware,
  // branch-stable way.
  const size_t len = std::strlen(result);
  if (len != h.size()) {
    return false;
  }
  unsigned char acc = 0;
  for (size_t i = 0; i < len; ++i) {
    acc |= static_cast<unsigned char>(result[i] ^ h[i]);
  }
  return acc == 0;
}

std::optional<std::string> LoadPasswordHash(const base::FilePath& path) {
  std::string contents;
  if (!base::ReadFileToString(path, &contents)) {
    return std::nullopt;
  }
  base::TrimWhitespaceASCII(contents, base::TRIM_TRAILING, &contents);
  if (contents.empty()) {
    return std::nullopt;
  }
  return contents;
}

bool SavePasswordHash(const base::FilePath& path, std::string_view hash) {
  base::CreateDirectory(path.DirName());
  const std::string data(hash);
  if (!base::ImportantFileWriter::WriteFileAtomically(path, data)) {
    return false;
  }
  // ImportantFileWriter doesn't let us set the final mode; chmod after.
  if (chmod(path.value().c_str(), 0600) != 0) {
    PLOG(WARNING) << "chmod 0600 " << path.value();
  }
  return true;
}

std::optional<std::string> LoadPasswordPlain(const base::FilePath& path) {
  std::string contents;
  if (!base::ReadFileToString(path, &contents)) {
    return std::nullopt;
  }
  base::TrimWhitespaceASCII(contents, base::TRIM_TRAILING, &contents);
  if (contents.empty()) {
    return std::nullopt;
  }
  return contents;
}

bool SavePasswordPlain(const base::FilePath& path,
                        std::string_view plaintext) {
  base::CreateDirectory(path.DirName());
  const std::string data(plaintext);
  if (!base::ImportantFileWriter::WriteFileAtomically(path, data)) {
    return false;
  }
  if (chmod(path.value().c_str(), 0600) != 0) {
    PLOG(WARNING) << "chmod 0600 " << path.value();
  }
  return true;
}

namespace {

// RFB 3.8 §7.2.2: each password byte is bit-reversed before being used
// as DES key bytes. Standard quirk.
uint8_t ReverseBits(uint8_t b) {
  uint8_t r = 0;
  for (int i = 0; i < 8; ++i) {
    r = static_cast<uint8_t>((r << 1) | (b & 1));
    b >>= 1;
  }
  return r;
}

}  // namespace

bool VerifyVncAuthResponse(std::string_view password,
                            const uint8_t challenge[16],
                            const uint8_t response[16]) {
  if (password.empty()) {
    return false;
  }
  // Build the 8-byte DES key from the password (truncate or pad with NUL),
  // then reverse the bit order of each byte.
  uint8_t key_bytes[8] = {};
  size_t n = std::min<size_t>(password.size(), 8);
  std::memcpy(key_bytes, password.data(), n);
  for (int i = 0; i < 8; ++i) {
    key_bytes[i] = ReverseBits(key_bytes[i]);
  }
  DES_key_schedule sched;
  DES_set_key_unchecked(reinterpret_cast<const_DES_cblock*>(key_bytes),
                        &sched);
  // Encrypt the 16-byte challenge as two ECB blocks; compare to response.
  uint8_t expected[16];
  DES_cblock block_in;
  DES_cblock block_out;
  std::memcpy(block_in, challenge, 8);
  DES_ecb_encrypt(&block_in, &block_out, &sched, DES_ENCRYPT);
  std::memcpy(expected, block_out, 8);
  std::memcpy(block_in, challenge + 8, 8);
  DES_ecb_encrypt(&block_in, &block_out, &sched, DES_ENCRYPT);
  std::memcpy(expected + 8, block_out, 8);
  // Constant-time-ish compare.
  uint8_t diff = 0;
  for (int i = 0; i < 16; ++i) {
    diff |= static_cast<uint8_t>(expected[i] ^ response[i]);
  }
  return diff == 0;
}

}  // namespace fydeos::vnc::net
