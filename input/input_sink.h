// Copyright 2026 The FydeOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FYDEOS_VNC_INPUT_INPUT_SINK_H_
#define FYDEOS_VNC_INPUT_INPUT_SINK_H_

#include <cstdint>
#include <string_view>

namespace fydeos::vnc::input {

// Abstract input sink. Implementations:
//   - UinputInputSink     (pre-login fallback, /dev/uinput)
//   - AshInputSink        (post-login primary, Mojo to Chrome)
//
// Keysyms are X11 keysym values (RFB carries them verbatim, RFC 6143 §7.5.4).
// Pointer coordinates are in the source frame's pre-rotation, pre-padding
// coordinate system; the sink is responsible for any device-side mapping.
class InputSink {
 public:
  InputSink() = default;
  InputSink(const InputSink&) = delete;
  InputSink& operator=(const InputSink&) = delete;
  virtual ~InputSink() = default;

  virtual void InjectKey(uint32_t keysym, bool down) = 0;

  // |button_mask| follows the RFB convention:
  //   bit 0 = left, bit 1 = middle, bit 2 = right,
  //   bit 3 = wheel up, bit 4 = wheel down,
  //   bit 5 = wheel left, bit 6 = wheel right.
  // Wheel bits arrive as transient "press" events from the viewer; the sink
  // translates them to REL_WHEEL/REL_HWHEEL deltas.
  virtual void InjectPointer(int x, int y, uint8_t button_mask) = 0;

  virtual void InjectClipboard(std::string_view utf8) {}  // v2
};

}  // namespace fydeos::vnc::input

#endif  // FYDEOS_VNC_INPUT_INPUT_SINK_H_
