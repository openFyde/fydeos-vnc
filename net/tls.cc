// Copyright 2026 The FydeOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "fydeos-vnc/net/tls.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <memory>
#include <string>
#include <vector>

#include <base/files/file_util.h>
#include <base/files/scoped_file.h>
#include <base/logging.h>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/sha.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

namespace fydeos::vnc::net {

namespace {

constexpr int kRsaBits = 4096;
constexpr long kValiditySeconds = 10LL * 365 * 24 * 3600;  // ~10 years

struct EvpPkeyDeleter {
  void operator()(EVP_PKEY* p) { EVP_PKEY_free(p); }
};
using ScopedEvpPkey = std::unique_ptr<EVP_PKEY, EvpPkeyDeleter>;

struct X509Deleter {
  void operator()(X509* x) { X509_free(x); }
};
using ScopedX509 = std::unique_ptr<X509, X509Deleter>;

struct BioDeleter {
  void operator()(BIO* b) { BIO_free_all(b); }
};
using ScopedBio = std::unique_ptr<BIO, BioDeleter>;

std::string GetHostname() {
  char buf[256] = {0};
  if (gethostname(buf, sizeof(buf) - 1) != 0) {
    return "fydeos";
  }
  return buf;
}

void LogOpenSslError(const std::string& where) {
  unsigned long err;
  while ((err = ERR_get_error()) != 0) {
    char buf[256];
    ERR_error_string_n(err, buf, sizeof(buf));
    LOG(ERROR) << where << ": " << buf;
  }
}

ScopedEvpPkey GenerateRsaKey() {
  EVP_PKEY* raw = EVP_RSA_gen(kRsaBits);
  if (!raw) {
    LogOpenSslError("EVP_RSA_gen");
    return nullptr;
  }
  return ScopedEvpPkey(raw);
}

bool AddSanDns(X509* cert, const std::string& dns) {
  std::string ext = "DNS:" + dns;
  X509_EXTENSION* xe = X509V3_EXT_conf_nid(
      nullptr, nullptr, NID_subject_alt_name, ext.c_str());
  if (!xe) {
    LogOpenSslError("X509V3_EXT_conf_nid SAN");
    return false;
  }
  const bool ok = X509_add_ext(cert, xe, -1) == 1;
  X509_EXTENSION_free(xe);
  return ok;
}

ScopedX509 BuildSelfSignedCert(EVP_PKEY* key, const std::string& cn) {
  ScopedX509 cert(X509_new());
  if (!cert) {
    return nullptr;
  }

  X509_set_version(cert.get(), 2);  // v3

  // Random 64-bit serial.
  ASN1_INTEGER* serial = ASN1_INTEGER_new();
  unsigned char bytes[8];
  base::ScopedFD urandom(open("/dev/urandom", O_RDONLY | O_CLOEXEC));
  if (!urandom.is_valid() ||
      read(urandom.get(), bytes, sizeof(bytes)) != sizeof(bytes)) {
    LOG(ERROR) << "Failed to read /dev/urandom for cert serial";
    ASN1_INTEGER_free(serial);
    return nullptr;
  }
  // Clear the top bit so the integer is positive.
  bytes[0] &= 0x7f;
  BIGNUM* bn = BN_bin2bn(bytes, sizeof(bytes), nullptr);
  BN_to_ASN1_INTEGER(bn, serial);
  BN_free(bn);
  X509_set_serialNumber(cert.get(), serial);
  ASN1_INTEGER_free(serial);

  X509_gmtime_adj(X509_get_notBefore(cert.get()), 0);
  X509_gmtime_adj(X509_get_notAfter(cert.get()), kValiditySeconds);

  X509_set_pubkey(cert.get(), key);

  X509_NAME* name = X509_get_subject_name(cert.get());
  X509_NAME_add_entry_by_txt(
      name, "CN", MBSTRING_ASC,
      reinterpret_cast<const unsigned char*>(cn.c_str()), -1, -1, 0);
  X509_set_issuer_name(cert.get(), name);

  if (!AddSanDns(cert.get(), cn)) {
    return nullptr;
  }

  if (!X509_sign(cert.get(), key, EVP_sha256())) {
    LogOpenSslError("X509_sign");
    return nullptr;
  }

  return cert;
}

bool WritePemFile(const base::FilePath& path,
                  mode_t mode,
                  const std::vector<uint8_t>& content) {
  base::ScopedFD fd(open(path.value().c_str(),
                          O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode));
  if (!fd.is_valid()) {
    PLOG(ERROR) << "open " << path.value();
    return false;
  }
  ssize_t off = 0;
  while (static_cast<size_t>(off) < content.size()) {
    ssize_t n = write(fd.get(), content.data() + off, content.size() - off);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      PLOG(ERROR) << "write " << path.value();
      return false;
    }
    off += n;
  }
  // fchmod again in case umask was wider.
  fchmod(fd.get(), mode);
  return true;
}

std::vector<uint8_t> DrainBio(BIO* bio) {
  char* data = nullptr;
  long len = BIO_get_mem_data(bio, &data);
  if (len <= 0) {
    return {};
  }
  return std::vector<uint8_t>(data, data + len);
}

}  // namespace

bool EnsureSelfSignedCertificate(const base::FilePath& cert_path,
                                 const base::FilePath& key_path) {
  if (base::PathExists(cert_path) && base::PathExists(key_path)) {
    return true;
  }

  ScopedEvpPkey key = GenerateRsaKey();
  if (!key) {
    return false;
  }

  ScopedX509 cert = BuildSelfSignedCert(key.get(), GetHostname());
  if (!cert) {
    return false;
  }

  ScopedBio cert_bio(BIO_new(BIO_s_mem()));
  ScopedBio key_bio(BIO_new(BIO_s_mem()));
  if (!cert_bio || !key_bio) {
    return false;
  }
  if (!PEM_write_bio_X509(cert_bio.get(), cert.get())) {
    LogOpenSslError("PEM_write_bio_X509");
    return false;
  }
  if (!PEM_write_bio_PrivateKey(key_bio.get(), key.get(), nullptr, nullptr,
                                 0, nullptr, nullptr)) {
    LogOpenSslError("PEM_write_bio_PrivateKey");
    return false;
  }

  // Make sure the parent dir exists.
  base::CreateDirectory(cert_path.DirName());

  if (!WritePemFile(cert_path, 0644, DrainBio(cert_bio.get())) ||
      !WritePemFile(key_path, 0600, DrainBio(key_bio.get()))) {
    return false;
  }
  LOG(INFO) << "Generated self-signed cert for "
            << GetHostname() << " at " << cert_path.value();
  return true;
}

std::string GetCertificateFingerprint(const base::FilePath& cert_path) {
  ScopedBio bio(BIO_new_file(cert_path.value().c_str(), "r"));
  if (!bio) {
    return {};
  }
  ScopedX509 cert(PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr));
  if (!cert) {
    LogOpenSslError("PEM_read_bio_X509");
    return {};
  }
  unsigned char md[SHA256_DIGEST_LENGTH];
  unsigned int len = sizeof(md);
  if (!X509_digest(cert.get(), EVP_sha256(), md, &len)) {
    LogOpenSslError("X509_digest");
    return {};
  }
  static const char kHex[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(len * 3);
  for (unsigned int i = 0; i < len; ++i) {
    if (i) {
      out.push_back(':');
    }
    out.push_back(kHex[md[i] >> 4]);
    out.push_back(kHex[md[i] & 0xf]);
  }
  return out;
}

SSL_CTX* MakeServerSSLContext(const base::FilePath& cert_path,
                              const base::FilePath& key_path) {
  SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());
  if (!ctx) {
    LogOpenSslError("SSL_CTX_new");
    return nullptr;
  }
  // TigerVNC, RealVNC, and modern viewers all speak TLS 1.2+; refuse
  // older protocols entirely.
  if (SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION) != 1) {
    LogOpenSslError("SSL_CTX_set_min_proto_version");
    SSL_CTX_free(ctx);
    return nullptr;
  }
  // Each VNC client connection is short-lived enough that session
  // resumption isn't worth the state.
  SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_OFF);
  SSL_CTX_set_options(ctx,
                      SSL_OP_NO_COMPRESSION | SSL_OP_CIPHER_SERVER_PREFERENCE);

  if (SSL_CTX_use_certificate_file(ctx, cert_path.value().c_str(),
                                   SSL_FILETYPE_PEM) != 1) {
    LogOpenSslError("SSL_CTX_use_certificate_file");
    SSL_CTX_free(ctx);
    return nullptr;
  }
  if (SSL_CTX_use_PrivateKey_file(ctx, key_path.value().c_str(),
                                  SSL_FILETYPE_PEM) != 1) {
    LogOpenSslError("SSL_CTX_use_PrivateKey_file");
    SSL_CTX_free(ctx);
    return nullptr;
  }
  if (SSL_CTX_check_private_key(ctx) != 1) {
    LogOpenSslError("SSL_CTX_check_private_key");
    SSL_CTX_free(ctx);
    return nullptr;
  }
  return ctx;
}

}  // namespace fydeos::vnc::net
