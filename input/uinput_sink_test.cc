// Copyright 2026 The FydeOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Tests for the input subsystem. /dev/uinput isn't available inside the
// platform2 test sandbox, so UinputInputSink itself is exercised on-device.
// Here we only cover the keysym map, which is pure userspace.

#include "fydeos-vnc/input/keysym_map.h"

#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <gtest/gtest.h>

namespace fydeos::vnc::input {

TEST(KeysymMapTest, UsLayoutBasicAscii) {
  KeysymMap m({}, {}, std::string("us"), {}, {});
  // ASCII letters should resolve to their KEY_* codes.
  EXPECT_EQ(KEY_A, m.Lookup(XKB_KEY_a));
  EXPECT_EQ(KEY_Z, m.Lookup(XKB_KEY_z));
  EXPECT_EQ(KEY_1, m.Lookup(XKB_KEY_1));
  EXPECT_EQ(KEY_SPACE, m.Lookup(XKB_KEY_space));
  EXPECT_EQ(KEY_ENTER, m.Lookup(XKB_KEY_Return));
  EXPECT_EQ(KEY_BACKSPACE, m.Lookup(XKB_KEY_BackSpace));
  EXPECT_EQ(KEY_ESC, m.Lookup(XKB_KEY_Escape));
}

TEST(KeysymMapTest, ModifierKeys) {
  KeysymMap m({}, {}, std::string("us"), {}, {});
  EXPECT_EQ(KEY_LEFTSHIFT, m.Lookup(XKB_KEY_Shift_L));
  EXPECT_EQ(KEY_RIGHTSHIFT, m.Lookup(XKB_KEY_Shift_R));
  EXPECT_EQ(KEY_LEFTCTRL, m.Lookup(XKB_KEY_Control_L));
  EXPECT_EQ(KEY_LEFTALT, m.Lookup(XKB_KEY_Alt_L));
}

TEST(KeysymMapTest, ShiftedKeysReportNeedsShift) {
  KeysymMap m({}, {}, std::string("us"), {}, {});
  bool needs_shift = false;
  EXPECT_EQ(KEY_A, m.Lookup(XKB_KEY_A, &needs_shift));
  EXPECT_TRUE(needs_shift);
  EXPECT_EQ(KEY_A, m.Lookup(XKB_KEY_a, &needs_shift));
  EXPECT_FALSE(needs_shift);
}

TEST(KeysymMapTest, UnknownKeysymReturnsZero) {
  KeysymMap m({}, {}, std::string("us"), {}, {});
  EXPECT_EQ(0, m.Lookup(0xdeadbeef));
}

}  // namespace fydeos::vnc::input
