// Copyright 2026 The FydeOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FYDEOS_VNC_CAPTURE_KMS_CAPTURE_SOURCE_H_
#define FYDEOS_VNC_CAPTURE_KMS_CAPTURE_SOURCE_H_

#include <atomic>
#include <memory>
#include <thread>

#include <base/memory/scoped_refptr.h>
#include <base/synchronization/waitable_event.h>
#include <base/task/single_thread_task_runner.h>

#include "fydeos-vnc/capture/capture_source.h"
#include "fydeos-vnc/capture/crtc.h"
#include "fydeos-vnc/capture/egl_capture.h"

namespace fydeos::vnc::capture {

// Pre-login fallback capture path. Wraps the upstream KMS+EGL readback into
// the CaptureSource async-callback shape. Damage is always reported as full
// frame; framerate is capped (default 5 fps) per design O-2.
class KmsCaptureSource : public CaptureSource {
 public:
  // |spec| selects which CRTC to follow. kInternalDisplay is the right
  // default for laptops; kAnyDisplay for headless betty-style VMs.
  explicit KmsCaptureSource(CrtcFinder::Spec spec = CrtcFinder::Spec::kAnyDisplay);
  ~KmsCaptureSource() override;

  bool Start(FrameCallback cb) override;
  void Stop() override;
  void Pause() override;
  void Resume() override;
  void SetTargetFps(int fps) override;

 private:
  void CaptureThreadMain();

  const CrtcFinder::Spec spec_;
  scoped_refptr<base::SingleThreadTaskRunner> origin_task_runner_;

  // Owned by the capture thread once Start() returns true.
  std::unique_ptr<Crtc> crtc_;
  std::unique_ptr<EglDisplayBuffer> egl_;

  FrameCallback callback_;
  std::atomic<int> target_fps_{5};
  std::atomic<bool> stop_requested_{false};
  // Toggled by Pause/Resume. The capture thread checks at the top of
  // each loop iteration; when set, it blocks on |stop_event_| (which
  // also gets signaled by Resume) instead of running Capture().
  std::atomic<bool> paused_{false};
  // Signaled by Stop() so the capture thread's between-frame sleeps
  // (CRTC retry + fps throttle) wake up promptly instead of holding
  // Stop() for up to a full retry interval. The vblank wait inside
  // EglDisplayBuffer::Capture() still has its own kernel-side bound
  // (~one frame at the panel refresh rate), which is acceptable.
  base::WaitableEvent stop_event_{
      base::WaitableEvent::ResetPolicy::MANUAL,
      base::WaitableEvent::InitialState::NOT_SIGNALED};
  std::thread capture_thread_;
};

}  // namespace fydeos::vnc::capture

#endif  // FYDEOS_VNC_CAPTURE_KMS_CAPTURE_SOURCE_H_
