// Copyright 2026 The FydeOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FYDEOS_VNC_INPUT_KEYSYM_MAP_H_
#define FYDEOS_VNC_INPUT_KEYSYM_MAP_H_

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>

namespace fydeos::vnc::input {

// Resolves X11 keysyms to Linux input event scancodes (KEY_*) by walking the
// active xkb_keymap (upstream kmsvnc used a hardcoded US-layout table).
//
// The map is built once at construction from the system's default keymap
// (XKB_DEFAULT_LAYOUT / XKB_DEFAULT_VARIANT env, falling back to "us"). It
// is not reactive to runtime layout changes — that's handled post-login by
// the Ash injector path.
class KeysymMap {
 public:
  // Build using a specific RMLVO triple. Pass nullopt for any of them to use
  // the libxkbcommon defaults (which honor XKB_DEFAULT_* env vars).
  KeysymMap(std::optional<std::string> rules,
            std::optional<std::string> model,
            std::optional<std::string> layout,
            std::optional<std::string> variant,
            std::optional<std::string> options);
  KeysymMap();
  KeysymMap(const KeysymMap&) = delete;
  KeysymMap& operator=(const KeysymMap&) = delete;
  ~KeysymMap();

  // Returns the Linux input event code (KEY_*) for |keysym|, or 0 if unknown.
  // |needs_shift| reports whether the keysym requires Shift to produce.
  uint16_t Lookup(uint32_t keysym, bool* needs_shift = nullptr) const;

 private:
  struct Entry {
    uint16_t scancode;
    bool needs_shift;
  };

  // X11 keysym → (Linux KEY_*, shifted?). Built from the loaded keymap; if
  // multiple keycodes produce the same keysym we keep the unshifted one.
  std::unordered_map<uint32_t, Entry> table_;
};

}  // namespace fydeos::vnc::input

#endif  // FYDEOS_VNC_INPUT_KEYSYM_MAP_H_
