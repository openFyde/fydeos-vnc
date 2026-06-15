// Copyright 2026 The FydeOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "fydeos-vnc/encoder/vnc_utils.h"

#include <cstring>

#include <base/check_op.h>

namespace fydeos::vnc::encoder {

namespace {
constexpr uint32_t kBytesPerPixel = 4;
}

uint32_t GetVncWidth(uint32_t source_width) {
  const uint32_t w = (source_width + 3) / 4 * 4;
  CHECK_GE(w, source_width);
  CHECK_LT(w - source_width, 4u);
  return w;
}

void ConvertBuffer(const capture::Frame& from,
                   uint32_t* to,
                   uint32_t vnc_width) {
  const auto* src = static_cast<const char*>(from.pixels);
  for (uint32_t row = 0; row < from.height; ++row) {
    std::memcpy(to + vnc_width * row,
                src + from.stride * row,
                from.width * kBytesPerPixel);
  }
}

}  // namespace fydeos::vnc::encoder
