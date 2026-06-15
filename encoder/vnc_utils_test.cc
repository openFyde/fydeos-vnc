// Copyright 2026 The FydeOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "fydeos-vnc/encoder/vnc_utils.h"

#include <gtest/gtest.h>

#include <vector>

namespace fydeos::vnc::encoder {

TEST(VncUtilsTest, GetVncWidthRoundsUpToMultipleOfFour) {
  EXPECT_EQ(4u, GetVncWidth(1));
  EXPECT_EQ(4u, GetVncWidth(4));
  EXPECT_EQ(8u, GetVncWidth(5));
  EXPECT_EQ(1368u, GetVncWidth(1366));
  EXPECT_EQ(1920u, GetVncWidth(1920));
  EXPECT_EQ(1920u, GetVncWidth(1919));
}

TEST(VncUtilsTest, ConvertBufferCopiesPaddedRows) {
  // Source 3x2 BGRA, stride = 12 bytes.
  std::vector<uint32_t> src = {
      0xAABBCCDDu, 0x11223344u, 0x55667788u,
      0x99AABBCCu, 0xDDEEFF00u, 0x01020304u,
  };
  capture::Frame f;
  f.width = 3;
  f.height = 2;
  f.stride = 3 * sizeof(uint32_t);
  f.pixels = src.data();

  // VNC width = 4. Output buffer initialized to a sentinel; row padding pixel
  // must remain untouched.
  std::vector<uint32_t> dst(4 * 2, 0xFFFFFFFFu);
  ConvertBuffer(f, dst.data(), 4);

  EXPECT_EQ(0xAABBCCDDu, dst[0]);
  EXPECT_EQ(0x11223344u, dst[1]);
  EXPECT_EQ(0x55667788u, dst[2]);
  EXPECT_EQ(0xFFFFFFFFu, dst[3]);  // padding untouched
  EXPECT_EQ(0x99AABBCCu, dst[4]);
  EXPECT_EQ(0xDDEEFF00u, dst[5]);
  EXPECT_EQ(0x01020304u, dst[6]);
  EXPECT_EQ(0xFFFFFFFFu, dst[7]);  // padding untouched
}

}  // namespace fydeos::vnc::encoder
