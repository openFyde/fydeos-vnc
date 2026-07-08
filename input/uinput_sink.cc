// Copyright 2026 The FydeOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "fydeos-vnc/input/uinput_sink.h"

#include <fcntl.h>
#include <linux/uinput.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include <base/logging.h>
#include <base/posix/eintr_wrapper.h>

namespace fydeos::vnc::input {

namespace {

// RFB button mask bit layout (see InputSink::InjectPointer comment).
constexpr uint8_t kBtnLeftBit = 1 << 0;
constexpr uint8_t kBtnMiddleBit = 1 << 1;
constexpr uint8_t kBtnRightBit = 1 << 2;
constexpr uint8_t kWheelUpBit = 1 << 3;
constexpr uint8_t kWheelDownBit = 1 << 4;
constexpr uint8_t kWheelLeftBit = 1 << 5;
constexpr uint8_t kWheelRightBit = 1 << 6;

bool Ioctl(int fd, unsigned long req) {  // NOLINT(runtime/int)
  if (HANDLE_EINTR(ioctl(fd, req)) < 0) {
    PLOG(ERROR) << "ioctl(" << req << ") failed";
    return false;
  }
  return true;
}

template <typename T>
bool Ioctl(int fd, unsigned long req, T arg) {  // NOLINT(runtime/int)
  if (HANDLE_EINTR(ioctl(fd, req, arg)) < 0) {
    PLOG(ERROR) << "ioctl(" << req << ") failed";
    return false;
  }
  return true;
}

void Emit(int fd, uint16_t type, uint16_t code, int32_t value) {
  input_event ev{};
  gettimeofday(&ev.time, nullptr);
  ev.type = type;
  ev.code = code;
  ev.value = value;
  if (HANDLE_EINTR(write(fd, &ev, sizeof(ev))) != static_cast<ssize_t>(sizeof(ev))) {
    PLOG(ERROR) << "write(uinput) failed";
  }
}

}  // namespace

UinputInputSink::UinputInputSink(uint32_t width, uint32_t height)
    : width_(width), height_(height) {
  ready_ = SetupKeyboard() && SetupPointer();
  if (!ready_) {
    LOG(ERROR) << "UinputInputSink failed to initialize; events will be dropped";
  }
}

UinputInputSink::~UinputInputSink() {
  if (keyboard_created_ && keyboard_fd_.is_valid()) {
    Ioctl(keyboard_fd_.get(), UI_DEV_DESTROY);
  }
  if (pointer_created_ && pointer_fd_.is_valid()) {
    Ioctl(pointer_fd_.get(), UI_DEV_DESTROY);
  }
}

bool UinputInputSink::SetupKeyboard() {
  keyboard_fd_.reset(open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC));
  if (!keyboard_fd_.is_valid()) {
    PLOG(ERROR) << "open /dev/uinput (keyboard)";
    return false;
  }

  if (!Ioctl(keyboard_fd_.get(), UI_SET_EVBIT, EV_KEY)) {
    return false;
  }
  // Enable every Linux KEY_* code so that whatever the xkb keymap surfaces,
  // we can emit it. KEY_MAX is small enough (~767) for this to be cheap.
  for (int code = 1; code < KEY_MAX; ++code) {
    Ioctl(keyboard_fd_.get(), UI_SET_KEYBIT, code);  // ignore individual fail
  }

  uinput_setup usetup{};
  usetup.id.bustype = BUS_USB;
  std::strncpy(usetup.name, "fydeos-vnc keyboard", UINPUT_MAX_NAME_SIZE - 1);
  if (!Ioctl(keyboard_fd_.get(), UI_DEV_SETUP, &usetup)) {
    return false;
  }
  if (!Ioctl(keyboard_fd_.get(), UI_DEV_CREATE)) {
    return false;
  }
  keyboard_created_ = true;
  return true;
}

bool UinputInputSink::SetupPointer() {
  pointer_fd_.reset(open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC));
  if (!pointer_fd_.is_valid()) {
    PLOG(ERROR) << "open /dev/uinput (pointer)";
    return false;
  }

  Ioctl(pointer_fd_.get(), UI_SET_EVBIT, EV_KEY);
  Ioctl(pointer_fd_.get(), UI_SET_KEYBIT, BTN_LEFT);
  Ioctl(pointer_fd_.get(), UI_SET_KEYBIT, BTN_RIGHT);
  Ioctl(pointer_fd_.get(), UI_SET_KEYBIT, BTN_MIDDLE);

  Ioctl(pointer_fd_.get(), UI_SET_EVBIT, EV_ABS);
  Ioctl(pointer_fd_.get(), UI_SET_ABSBIT, ABS_X);
  Ioctl(pointer_fd_.get(), UI_SET_ABSBIT, ABS_Y);

  Ioctl(pointer_fd_.get(), UI_SET_EVBIT, EV_REL);
  Ioctl(pointer_fd_.get(), UI_SET_RELBIT, REL_WHEEL);
  Ioctl(pointer_fd_.get(), UI_SET_RELBIT, REL_HWHEEL);

  uinput_abs_setup uabs{};
  uabs.code = ABS_X;
  uabs.absinfo.maximum = static_cast<int32_t>(width_) - 1;
  Ioctl(pointer_fd_.get(), UI_ABS_SETUP, &uabs);
  uabs.code = ABS_Y;
  uabs.absinfo.maximum = static_cast<int32_t>(height_) - 1;
  Ioctl(pointer_fd_.get(), UI_ABS_SETUP, &uabs);

  uinput_setup usetup{};
  usetup.id.bustype = BUS_USB;
  std::strncpy(usetup.name, "fydeos-vnc pointer", UINPUT_MAX_NAME_SIZE - 1);
  if (!Ioctl(pointer_fd_.get(), UI_DEV_SETUP, &usetup)) {
    return false;
  }
  if (!Ioctl(pointer_fd_.get(), UI_DEV_CREATE)) {
    return false;
  }
  pointer_created_ = true;
  return true;
}

void UinputInputSink::InjectKey(uint32_t keysym, bool down) {
  if (!ready_) {
    return;
  }
  bool needs_shift = false;
  const uint16_t scancode = keysym_map_.Lookup(keysym, &needs_shift);
  if (!scancode) {
    VLOG(1) << "Dropping unknown keysym 0x" << std::hex << keysym;
    return;
  }
  // We honor needs_shift only when the viewer hasn't already sent its own
  // shift keysym. For now, defer to the viewer: if a viewer wants the '!'
  // character it will send Shift_L down before sending '!'. Auto-shifting
  // here would conflict with that. needs_shift is preserved for future
  // unicode-text injection (M+).
  (void)needs_shift;
  Emit(keyboard_fd_.get(), EV_KEY, scancode, down ? 1 : 0);
  Emit(keyboard_fd_.get(), EV_SYN, SYN_REPORT, 0);
}

void UinputInputSink::InjectPointer(int x, int y, uint8_t button_mask) {
  if (!ready_) {
    return;
  }

  // Clamp coordinates to the device range.
  if (x < 0) x = 0;
  if (y < 0) y = 0;
  if (static_cast<uint32_t>(x) >= width_) x = static_cast<int>(width_) - 1;
  if (static_cast<uint32_t>(y) >= height_) y = static_cast<int>(height_) - 1;

  Emit(pointer_fd_.get(), EV_ABS, ABS_X, x);
  Emit(pointer_fd_.get(), EV_ABS, ABS_Y, y);

  const uint8_t changed = button_mask ^ last_button_mask_;
  auto edge = [&](uint8_t bit, uint16_t code) {
    if (changed & bit) {
      Emit(pointer_fd_.get(), EV_KEY, code, (button_mask & bit) ? 1 : 0);
    }
  };
  edge(kBtnLeftBit, BTN_LEFT);
  edge(kBtnMiddleBit, BTN_MIDDLE);
  edge(kBtnRightBit, BTN_RIGHT);

  // Wheel bits are reported as transient presses by viewers — they arrive
  // set, then unset on the next event. Emit a wheel tick on each
  // transition-to-pressed edge.
  auto wheel = [&](uint8_t bit, uint16_t code, int32_t delta) {
    if ((changed & bit) && (button_mask & bit)) {
      Emit(pointer_fd_.get(), EV_REL, code, delta);
    }
  };
  wheel(kWheelUpBit, REL_WHEEL, 1);
  wheel(kWheelDownBit, REL_WHEEL, -1);
  wheel(kWheelLeftBit, REL_HWHEEL, -1);
  wheel(kWheelRightBit, REL_HWHEEL, 1);

  Emit(pointer_fd_.get(), EV_SYN, SYN_REPORT, 0);

  // Remember the full mask (buttons for press/release edges, wheel bits
  // so the next event sees their release edge).
  last_button_mask_ = button_mask;
}

}  // namespace fydeos::vnc::input
