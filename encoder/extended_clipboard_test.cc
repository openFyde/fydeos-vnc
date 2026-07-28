// Copyright 2026 The FydeOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "fydeos-vnc/encoder/extended_clipboard.h"

#include <gtest/gtest.h>

#include <string>

namespace fydeos::vnc::encoder::extclip {

TEST(ExtendedClipboardTest, ParsesCapsWithFlags) {
  std::string body = BuildCapsBody();
  auto msg = Parse(body.data(), body.size());
  EXPECT_EQ(InboundKind::kCaps, msg.kind);
  EXPECT_TRUE(msg.caps_flags & kActCaps);
  EXPECT_TRUE(msg.caps_flags & kActRequest);
  EXPECT_TRUE(msg.caps_flags & kActPeek);
  EXPECT_TRUE(msg.caps_flags & kActNotify);
  EXPECT_TRUE(msg.caps_flags & kActProvide);
  EXPECT_TRUE(msg.caps_flags & kFmtText);
}

TEST(ExtendedClipboardTest, ParsesRequestText) {
  std::string body = BuildRequestBody();
  auto msg = Parse(body.data(), body.size());
  EXPECT_EQ(InboundKind::kRequestText, msg.kind);
}

TEST(ExtendedClipboardTest, ParsesNotifyText) {
  std::string body = BuildNotifyBody();
  auto msg = Parse(body.data(), body.size());
  EXPECT_EQ(InboundKind::kNotifyText, msg.kind);
}

TEST(ExtendedClipboardTest, NotifyWithoutTextFormatIsIgnored) {
  std::string body = BuildNotifyBody(/*formats=*/0);
  auto msg = Parse(body.data(), body.size());
  EXPECT_EQ(InboundKind::kIgnored, msg.kind);
}

TEST(ExtendedClipboardTest, ParsesPeek) {
  // Peek is a bare flags word with only the action bit.
  const uint8_t raw[4] = {0x04, 0x00, 0x00, 0x00};  // 1 << 26
  auto msg = Parse(reinterpret_cast<const char*>(raw), sizeof(raw));
  EXPECT_EQ(InboundKind::kPeek, msg.kind);
}

TEST(ExtendedClipboardTest, ProvideRoundTripsThroughParse) {
  const std::string text = "hello 世界 ünïcode";
  std::string body = BuildProvideTextBody(text);
  ASSERT_FALSE(body.empty());
  auto msg = Parse(body.data(), body.size());
  ASSERT_EQ(InboundKind::kProvideText, msg.kind);
  // BuildProvideTextBody appends a counted trailing NUL (TigerVNC
  // convention); Parse strips it back off.
  EXPECT_EQ(text, msg.text);
}

TEST(ExtendedClipboardTest, ProvideRoundTripsEmptyText) {
  std::string body = BuildProvideTextBody("");
  ASSERT_FALSE(body.empty());
  auto msg = Parse(body.data(), body.size());
  ASSERT_EQ(InboundKind::kProvideText, msg.kind);
  EXPECT_TRUE(msg.text.empty());
}

TEST(ExtendedClipboardTest, ProvideRoundTripsLargeText) {
  const std::string text(1u << 20, 'x');  // 1 MB, matches advertised max
  std::string body = BuildProvideTextBody(text);
  ASSERT_FALSE(body.empty());
  // Compressible payload must actually compress.
  EXPECT_LT(body.size(), text.size() / 2);
  auto msg = Parse(body.data(), body.size());
  ASSERT_EQ(InboundKind::kProvideText, msg.kind);
  EXPECT_EQ(text, msg.text);
}

}  // namespace fydeos::vnc::encoder::extclip
