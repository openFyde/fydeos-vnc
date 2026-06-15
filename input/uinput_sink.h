// Copyright 2026 The FydeOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FYDEOS_VNC_INPUT_UINPUT_SINK_H_
#define FYDEOS_VNC_INPUT_UINPUT_SINK_H_

#include <cstdint>
#include <memory>

#include <base/files/scoped_file.h>

#include "fydeos-vnc/input/input_sink.h"
#include "fydeos-vnc/input/keysym_map.h"

namespace fydeos::vnc::input {

// Pre-login fallback input sink. Synthesizes events via /dev/uinput.
// Differences from upstream kmsvnc's uinput.cc (per design §5.3.2, O-8):
//   - Pointer device is a mouse (BTN_LEFT/RIGHT/MIDDLE + REL_WHEEL/HWHEEL +
//     EV_ABS for absolute X/Y), not a touchscreen with left-button-only.
//   - Keysym→scancode lookup goes through libxkbcommon (KeysymMap), not the
//     hardcoded US-layout table.
class UinputInputSink : public InputSink {
 public:
  // |width|, |height| are the source-frame dimensions used for the pointer's
  // ABS_X/Y maxima. Pass the same values the capture source emits.
  UinputInputSink(uint32_t width, uint32_t height);
  ~UinputInputSink() override;

  // True if both uinput devices were created successfully. If false,
  // InjectKey/InjectPointer are no-ops.
  bool ready() const { return ready_; }

  // InputSink:
  void InjectKey(uint32_t keysym, bool down) override;
  void InjectPointer(int x, int y, uint8_t button_mask) override;

 private:
  bool SetupKeyboard();
  bool SetupPointer();
  void EmitKey(int fd, uint16_t code, int32_t value) const;
  void EmitSyn(int fd) const;

  const uint32_t width_;
  const uint32_t height_;
  KeysymMap keysym_map_;

  base::ScopedFD keyboard_fd_;
  base::ScopedFD pointer_fd_;
  bool keyboard_created_ = false;
  bool pointer_created_ = false;
  bool ready_ = false;

  // Last button mask seen, so we can compute edges (press / release) and
  // wheel transitions.
  uint8_t last_button_mask_ = 0;
};

}  // namespace fydeos::vnc::input

#endif  // FYDEOS_VNC_INPUT_UINPUT_SINK_H_
