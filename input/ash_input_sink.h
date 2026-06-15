// Copyright 2026 The FydeOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FYDEOS_VNC_INPUT_ASH_INPUT_SINK_H_
#define FYDEOS_VNC_INPUT_ASH_INPUT_SINK_H_

#include <base/memory/raw_ptr.h>

#include "fydeos-vnc/input/input_sink.h"
#include "fydeos-vnc/input/keysym_map.h"

namespace fydeos::vnc::mojo_client {
class ChromeClient;
}

namespace fydeos::vnc::input {

// Post-login input sink. Forwards Inject* into Chrome via the Mojo
// InputInjectionService. Chrome side bridges to ui::EventInjector (M7b).
class AshInputSink : public InputSink {
 public:
  explicit AshInputSink(mojo_client::ChromeClient* chrome);
  ~AshInputSink() override;

  // InputSink:
  void InjectKey(uint32_t keysym, bool down) override;
  void InjectPointer(int x, int y, uint8_t button_mask) override;
  void InjectClipboard(std::string_view utf8) override;

 private:
  base::raw_ptr<mojo_client::ChromeClient> chrome_;
  // libxkbcommon-backed reverse map: X11 keysym → Linux KEY_* evdev code.
  // Same instance type the uinput sink uses; cheap to construct (one xkb
  // keymap walk).
  KeysymMap keysym_map_;
  uint8_t last_button_mask_ = 0;
};

}  // namespace fydeos::vnc::input

#endif  // FYDEOS_VNC_INPUT_ASH_INPUT_SINK_H_
