// Copyright 2026 The FydeOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "fydeos-vnc/input/keysym_map.h"

#include <utility>
#include <vector>

#include <base/logging.h>
#include <xkbcommon/xkbcommon.h>

namespace fydeos::vnc::input {

namespace {

// Linux input scancode = xkb keycode - 8 (X11 convention).
constexpr uint32_t kXkbKeycodeOffset = 8;

}  // namespace

KeysymMap::KeysymMap() : KeysymMap({}, {}, {}, {}, {}) {}

KeysymMap::KeysymMap(std::optional<std::string> rules,
                     std::optional<std::string> model,
                     std::optional<std::string> layout,
                     std::optional<std::string> variant,
                     std::optional<std::string> options) {
  xkb_context* ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
  if (!ctx) {
    LOG(ERROR) << "xkb_context_new failed; keysym map will be empty";
    return;
  }

  xkb_rule_names names{
      rules ? rules->c_str() : nullptr,
      model ? model->c_str() : nullptr,
      layout ? layout->c_str() : nullptr,
      variant ? variant->c_str() : nullptr,
      options ? options->c_str() : nullptr,
  };
  xkb_keymap* keymap = xkb_keymap_new_from_names(ctx, &names,
                                                  XKB_KEYMAP_COMPILE_NO_FLAGS);
  if (!keymap) {
    LOG(ERROR) << "xkb_keymap_new_from_names failed; keysym map will be empty";
    xkb_context_unref(ctx);
    return;
  }

  const xkb_keycode_t min_kc = xkb_keymap_min_keycode(keymap);
  const xkb_keycode_t max_kc = xkb_keymap_max_keycode(keymap);

  // We only care about layout 0 (the active layout). Level 0 = unshifted,
  // level 1 = shifted. If a keysym appears at both, keep the unshifted entry.
  for (xkb_keycode_t kc = min_kc; kc <= max_kc; ++kc) {
    if (kc < kXkbKeycodeOffset) {
      continue;
    }
    const uint16_t scancode =
        static_cast<uint16_t>(kc - kXkbKeycodeOffset);

    for (uint32_t level : {0u, 1u}) {
      const xkb_keysym_t* syms = nullptr;
      const int n = xkb_keymap_key_get_syms_by_level(
          keymap, kc, /*layout=*/0, level, &syms);
      for (int i = 0; i < n; ++i) {
        const uint32_t keysym = static_cast<uint32_t>(syms[i]);
        if (keysym == XKB_KEY_NoSymbol) {
          continue;
        }
        const bool needs_shift = (level == 1);
        auto [it, inserted] =
            table_.try_emplace(keysym, Entry{scancode, needs_shift});
        if (!inserted && !needs_shift && it->second.needs_shift) {
          // Prefer the unshifted producer if we previously stored a shifted
          // one (e.g. for keysyms that appear on multiple keys).
          it->second = Entry{scancode, false};
        }
      }
    }
  }

  xkb_keymap_unref(keymap);
  xkb_context_unref(ctx);

  VLOG(1) << "Built keysym map with " << table_.size() << " entries";
}

KeysymMap::~KeysymMap() = default;

uint16_t KeysymMap::Lookup(uint32_t keysym, bool* needs_shift) const {
  auto it = table_.find(keysym);
  if (it == table_.end()) {
    if (needs_shift) {
      *needs_shift = false;
    }
    return 0;
  }
  if (needs_shift) {
    *needs_shift = it->second.needs_shift;
  }
  return it->second.scancode;
}

}  // namespace fydeos::vnc::input
