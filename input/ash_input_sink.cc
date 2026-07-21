// Copyright 2026 The FydeOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "fydeos-vnc/input/ash_input_sink.h"

#include <base/check.h>
#include <base/logging.h>

#include "fydeos-vnc/mojo/chrome_client.h"

namespace fydeos::vnc::input {

namespace {

// Same RFB mask layout as UinputInputSink. We translate wheel-bit edges
// here into REL_WHEEL deltas so the Chrome-side InputInjectionService
// sees a consistent shape.
constexpr uint8_t kWheelUpBit = 1 << 3;
constexpr uint8_t kWheelDownBit = 1 << 4;
constexpr uint8_t kWheelLeftBit = 1 << 5;
constexpr uint8_t kWheelRightBit = 1 << 6;

}  // namespace

AshInputSink::AshInputSink(mojo_client::ChromeClient* c) : chrome_(c) {
  CHECK(chrome_);
}

AshInputSink::~AshInputSink() = default;

void AshInputSink::InjectKey(uint32_t keysym, bool down) {
  auto* svc = chrome_->input_injection();
  if (!svc) {
    return;
  }
  const uint16_t evdev = keysym_map_.Lookup(keysym);
  if (evdev == 0) {
    VLOG(1) << "AshInputSink: dropping unknown keysym 0x" << std::hex
            << keysym;
    return;
  }
  svc->InjectKey(static_cast<uint32_t>(evdev), down);
}

void AshInputSink::InjectClipboard(std::string_view utf8) {
  auto* svc = chrome_->input_injection();
  if (!svc) {
    return;
  }
  // Forward to chrome via Mojo; chrome bridges to ui::TextInputClient.
  svc->InjectText(std::string(utf8));
}

void AshInputSink::InjectPointer(int x, int y, uint8_t button_mask) {
  auto* svc = chrome_->input_injection();
  if (!svc) {
    return;
  }
  // Translate wheel-bit edge to a single wheel-delta tick.
  int8_t wheel_delta = 0;
  const uint8_t changed = button_mask ^ last_button_mask_;
  if ((changed & kWheelUpBit) && (button_mask & kWheelUpBit)) {
    wheel_delta = 1;
  } else if ((changed & kWheelDownBit) && (button_mask & kWheelDownBit)) {
    wheel_delta = -1;
  }
  // Horizontal wheel bits are accepted but not forwarded — the Mojo
  // interface carries a single vertical wheel_delta field.
  (void)kWheelLeftBit;
  (void)kWheelRightBit;

  svc->InjectPointer(x, y, button_mask, wheel_delta, display_id_);
  last_button_mask_ = button_mask;
}

}  // namespace fydeos::vnc::input
