// Copyright 2026 The FydeOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FYDEOS_VNC_CAPTURE_CAPTURE_SOURCE_H_
#define FYDEOS_VNC_CAPTURE_CAPTURE_SOURCE_H_

#include <cstdint>
#include <memory>
#include <vector>

#include <base/functional/callback.h>
#include <base/time/time.h>

namespace fydeos::vnc::capture {

// Damage region carried with each captured frame. An empty list means "no
// damage info available" (KMS source); the caller should treat that as
// full-frame.
struct DamageRect {
  uint32_t x;
  uint32_t y;
  uint32_t w;
  uint32_t h;
};

using DamageRegion = std::vector<DamageRect>;

// One captured BGRA8888 frame. |pixels| is borrowed from the source and is
// valid only until the callback returns; consumers must memcpy if they need
// to retain it.
struct Frame {
  uint32_t width;
  uint32_t height;
  uint32_t stride;  // in bytes
  const void* pixels;
  DamageRegion damage;
  base::TimeTicks captured_at;
};

// Abstract capture source. See design §5.2.1. Implementations:
//   - KmsCaptureSource     (pre-login fallback, KMS + EGL readback)
//   - FrameSinkCaptureSource (post-login primary, viz over Mojo) — M7
class CaptureSource {
 public:
  using FrameCallback =
      base::RepeatingCallback<void(std::unique_ptr<Frame>)>;

  CaptureSource() = default;
  CaptureSource(const CaptureSource&) = delete;
  CaptureSource& operator=(const CaptureSource&) = delete;
  virtual ~CaptureSource() = default;

  // Begin streaming frames to |cb|. Synchronous failure returns false; later
  // failures (mode change, source disappeared) are surfaced as a null frame.
  virtual bool Start(FrameCallback cb) = 0;
  virtual void Stop() = 0;

  // Soft pause / resume: keep the source object alive (so its OS thread /
  // Mojo channel survives a VNC toggle-off without the Mesa-EGL teardown
  // deadlock) but stop spending CPU / GPU on captures the daemon will
  // discard. Pause is idempotent; Resume restores normal streaming.
  // Default no-op for sources that don't have a cheap pause path.
  virtual void Pause() {}
  virtual void Resume() {}

  // Hint to the source about desired framerate. The source may ignore it; in
  // particular FrameSink obeys it via SetMinCapturePeriod().
  virtual void SetTargetFps(int fps) = 0;
};

}  // namespace fydeos::vnc::capture

#endif  // FYDEOS_VNC_CAPTURE_CAPTURE_SOURCE_H_
