// Copyright 2026 The FydeOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FYDEOS_VNC_ENCODER_VNC_UTILS_H_
#define FYDEOS_VNC_ENCODER_VNC_UTILS_H_

#include <cstdint>

#include "fydeos-vnc/capture/capture_source.h"

namespace fydeos::vnc::encoder {

// libvncserver requires framebuffer width to be a multiple of 4. Round up.
uint32_t GetVncWidth(uint32_t source_width);

// Copy |from| into a libvncserver-shaped buffer of stride |vnc_width| BGRA
// pixels. Padding pixels (the rightmost up-to-3 columns) are left untouched.
void ConvertBuffer(const capture::Frame& from,
                   uint32_t* to,
                   uint32_t vnc_width);

}  // namespace fydeos::vnc::encoder

#endif  // FYDEOS_VNC_ENCODER_VNC_UTILS_H_
