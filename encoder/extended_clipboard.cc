// Copyright 2026 The FydeOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "fydeos-vnc/encoder/extended_clipboard.h"

#include <arpa/inet.h>
#include <zlib.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include <base/logging.h>

namespace fydeos::vnc::encoder::extclip {

namespace {

// Read a big-endian u32 from |p| (no bounds check; caller must ensure
// |p| has at least 4 readable bytes).
uint32_t ReadBeU32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) |
         (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

void WriteBeU32(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v >> 24);
  p[1] = static_cast<uint8_t>(v >> 16);
  p[2] = static_cast<uint8_t>(v >> 8);
  p[3] = static_cast<uint8_t>(v);
}

}  // namespace

InboundMessage Parse(const char* data, size_t len) {
  InboundMessage out;
  if (len < 4) {
    return out;
  }
  const uint8_t* p = reinterpret_cast<const uint8_t*>(data);
  uint32_t flags = ReadBeU32(p);
  p += 4;
  size_t remaining = len - 4;

  // High byte holds action bits; low 16 bits hold format bits.
  // Caps response can have multiple action bits set (the client lists
  // which actions IT supports). We branch on Provide first only if
  // Caps is NOT set, because a caps message can technically also have
  // Provide bit listed.
  const bool is_caps = (flags & kActCaps) != 0;
  const bool is_notify = (flags & kActNotify) != 0;
  const bool is_provide = (flags & kActProvide) != 0;

  if (is_caps) {
    VLOG(1) << "ExtClip: client caps flags=0x" << std::hex << flags;
    out.kind = InboundKind::kCaps;
    out.caps_flags = flags;
    return out;
  }
  if (flags & kActRequest) {
    if (flags & kFmtText) {
      out.kind = InboundKind::kRequestText;
    }
    return out;
  }
  if (flags & kActPeek) {
    out.kind = InboundKind::kPeek;
    return out;
  }
  if (is_notify) {
    if (flags & kFmtText) {
      out.kind = InboundKind::kNotifyText;
    }
    return out;
  }
  if (!is_provide) {
    return out;
  }
  if (!(flags & kFmtText)) {
    return out;
  }

  // Provide TEXT: body after flags is zlib-compressed; decompressed
  // stream begins with u32 text length + that many UTF-8 bytes (one
  // length+blob per format bit, in the order TEXT, RTF, HTML, DIB,
  // Files; we only consume TEXT).
  z_stream zs;
  std::memset(&zs, 0, sizeof(zs));
  if (inflateInit(&zs) != Z_OK) {
    LOG(WARNING) << "ExtClip: inflateInit failed";
    return out;
  }
  zs.next_in = const_cast<Bytef*>(p);
  zs.avail_in = static_cast<uInt>(remaining);

  // Decompress into a bounded buffer. Cap at 4 MB output, matching the
  // 10 MB input cap with rough 2-4x typical compression ratio + safety.
  constexpr size_t kMaxOutput = 4u << 20;
  std::vector<uint8_t> buf(kMaxOutput);
  zs.next_out = buf.data();
  zs.avail_out = static_cast<uInt>(buf.size());

  int rc = inflate(&zs, Z_SYNC_FLUSH);
  size_t produced = buf.size() - zs.avail_out;
  inflateEnd(&zs);
  if (rc != Z_STREAM_END && rc != Z_OK && rc != Z_BUF_ERROR) {
    LOG(WARNING) << "ExtClip: zlib inflate failed rc=" << rc;
    return out;
  }
  if (produced < 4) {
    return out;
  }
  uint32_t text_len = ReadBeU32(buf.data());
  if (text_len > produced - 4) {
    LOG(WARNING) << "ExtClip: text_len=" << text_len
                 << " exceeds decompressed body " << (produced - 4);
    return out;
  }
  // TigerVNC includes a trailing NUL in the byte count for some
  // clients; strip it so chrome doesn't insert a literal \0.
  size_t real_len = text_len;
  if (real_len > 0 && buf[4 + real_len - 1] == 0) {
    --real_len;
  }
  out.kind = InboundKind::kProvideText;
  out.text.assign(reinterpret_cast<const char*>(buf.data() + 4), real_len);
  return out;
}

std::string BuildRequestBody(uint32_t formats) {
  std::string body;
  body.resize(4);
  WriteBeU32(reinterpret_cast<uint8_t*>(body.data()),
             kActRequest | (formats & 0xFFFFu));
  return body;
}

std::string BuildNotifyBody(uint32_t formats) {
  std::string body;
  body.resize(4);
  WriteBeU32(reinterpret_cast<uint8_t*>(body.data()),
             kActNotify | (formats & 0xFFFFu));
  return body;
}

std::string BuildProvideTextBody(std::string_view utf8) {
  // Uncompressed stream: u32 byte count + UTF-8 bytes + trailing NUL
  // (counted — TigerVNC-compatible; receivers strip it).
  std::vector<uint8_t> plain(4 + utf8.size() + 1);
  WriteBeU32(plain.data(), static_cast<uint32_t>(utf8.size() + 1));
  std::memcpy(plain.data() + 4, utf8.data(), utf8.size());
  plain[4 + utf8.size()] = 0;

  std::string body;
  uLongf comp_len = compressBound(static_cast<uLong>(plain.size()));
  body.resize(4 + comp_len);
  uint8_t* p = reinterpret_cast<uint8_t*>(body.data());
  WriteBeU32(p, kActProvide | kFmtText);
  if (compress2(p + 4, &comp_len, plain.data(),
                static_cast<uLong>(plain.size()),
                Z_DEFAULT_COMPRESSION) != Z_OK) {
    LOG(WARNING) << "ExtClip: zlib compress failed";
    return std::string();
  }
  body.resize(4 + comp_len);
  return body;
}

std::string BuildCapsBody(uint32_t supported_formats) {
  // Caps body: u32 flags + per-format u32 max-size for each advertised
  // format. The action bits in |flags| advertise *which actions* the
  // server accepts from the peer; the format bits advertise *which
  // formats*. We accept all of them for TEXT: caps, request (we answer
  // with provide), peek (we answer with notify), notify (we answer
  // with request) and provide (we inject the text).
  std::string body;
  body.resize(8);
  uint8_t* p = reinterpret_cast<uint8_t*>(body.data());
  uint32_t flags = kActCaps | kActRequest | kActPeek | kActNotify |
                   kActProvide | (supported_formats & 0xFFFFu);
  WriteBeU32(p, flags);
  WriteBeU32(p + 4, 1u << 20);  // max text size = 1 MB
  return body;
}

}  // namespace fydeos::vnc::encoder::extclip
