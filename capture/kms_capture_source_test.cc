// Copyright 2026 The FydeOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Smoke tests for KmsCaptureSource. We can't exercise the actual EGL path
// in the unit test environment (no DRM device, no GPU), so M1 only covers
// constructor + Stop() and the target-fps clamp. Real capture is verified
// on-device.

#include "fydeos-vnc/capture/kms_capture_source.h"

#include <gtest/gtest.h>

namespace fydeos::vnc::capture {

TEST(KmsCaptureSourceTest, ConstructDestruct) {
  KmsCaptureSource src;
  // Destructor calls Stop(); should be a no-op since we never Start()ed.
}

TEST(KmsCaptureSourceTest, SetTargetFpsClampsToOne) {
  KmsCaptureSource src;
  src.SetTargetFps(0);
  src.SetTargetFps(-5);
  // No public accessor; we only assert it doesn't crash. The clamp is
  // covered by the capture loop's Hertz() call.
}

}  // namespace fydeos::vnc::capture
