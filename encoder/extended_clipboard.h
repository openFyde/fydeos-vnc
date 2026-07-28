// Copyright 2026 The FydeOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FYDEOS_VNC_ENCODER_EXTENDED_CLIPBOARD_H_
#define FYDEOS_VNC_ENCODER_EXTENDED_CLIPBOARD_H_

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace fydeos::vnc::encoder::extclip {

// TigerVNC Extended Clipboard pseudo-encoding number sent by clients
// in their SetEncodings list to advertise support. Note: the value on
// the wire is 0xC0A1E5CE; many docs misquote the decimal — the actual
// signed int32 interpretation is -1063131698.
constexpr int32_t kPseudoEncoding = static_cast<int32_t>(0xC0A1E5CE);

// Format bits (low 16 bits of the flags word).
constexpr uint32_t kFmtText = 1u << 0;
constexpr uint32_t kFmtRtf = 1u << 1;
constexpr uint32_t kFmtHtml = 1u << 2;
constexpr uint32_t kFmtDib = 1u << 3;
constexpr uint32_t kFmtFiles = 1u << 4;

// Action bits (high byte).
constexpr uint32_t kActCaps = 1u << 24;
constexpr uint32_t kActRequest = 1u << 25;
constexpr uint32_t kActPeek = 1u << 26;
constexpr uint32_t kActNotify = 1u << 27;
constexpr uint32_t kActProvide = 1u << 28;

// Classification of an inbound Extended Clipboard message.
enum class InboundKind {
  kIgnored,        // unsupported flags / malformed
  kCaps,           // client caps message (caps_flags populated)
  kNotifyText,     // client has TEXT clipboard content available
  kProvideText,    // client is sending TEXT content (text_out populated)
  kRequestText,    // client asks the server to Provide its TEXT clipboard
  kPeek,           // client asks the server to re-send a Notify
};

struct InboundMessage {
  InboundKind kind = InboundKind::kIgnored;
  std::string text;        // populated iff kind == kProvideText
  uint32_t caps_flags = 0; // populated iff kind == kCaps (raw flags word)
};

// Parses an Extended Clipboard payload that the server received from a
// client (the body of a ClientCutText message with negative length).
//
// |data| points at the message body (just past the 8-byte
// ClientCutText header). |len| is the size of the body in bytes (=
// abs(signed length on wire)).
InboundMessage Parse(const char* data, size_t len);

// Builds the body of an Extended Clipboard `caps` message announcing
// the server's supported formats (currently just TEXT). The returned
// bytes get prepended with the standard 8-byte ClientCutText header
// (type=6, pad[3], int32 -len) and written to the client socket.
std::string BuildCapsBody(uint32_t supported_formats = kFmtText);

// Builds the body of a `request` message asking the client to send
// the named formats (typically TEXT).
std::string BuildRequestBody(uint32_t formats = kFmtText);

// Builds the body of a `notify` message announcing which formats the
// server now has clipboard content for (0 = clipboard cleared /
// nothing available).
std::string BuildNotifyBody(uint32_t formats = kFmtText);

// Builds the body of a `provide` message carrying |utf8| as the TEXT
// format: u32 flags + zlib-compressed (u32 byte count + UTF-8 bytes).
// The byte count includes a trailing NUL, matching TigerVNC's server
// behavior (our own Parse strips it back off).
std::string BuildProvideTextBody(std::string_view utf8);

}  // namespace fydeos::vnc::encoder::extclip

#endif  // FYDEOS_VNC_ENCODER_EXTENDED_CLIPBOARD_H_
