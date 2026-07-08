// Copyright 2026 The FydeOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "fydeos-vnc/capture/kms_capture_source.h"

#include <utility>

#include <cstring>

#include <base/check.h>
#include <base/functional/bind.h>
#include <base/logging.h>
#include <base/task/single_thread_task_runner.h>
#include <base/threading/platform_thread.h>
#include <base/time/time.h>

namespace fydeos::vnc::capture {

namespace {

// Maximum interval the capture thread will sleep between attempts when no
// CRTC is present at startup (terminal lid closed, DPMS off, etc.). Lower
// values waste CPU; higher values delay reattach after wakeup.
constexpr base::TimeDelta kCrtcRetryInterval = base::Milliseconds(500);

}  // namespace

KmsCaptureSource::KmsCaptureSource(CrtcFinder::Spec spec) : spec_(spec) {}

KmsCaptureSource::~KmsCaptureSource() {
  Stop();
}

bool KmsCaptureSource::Start(FrameCallback cb) {
  CHECK(!capture_thread_.joinable()) << "Start() called twice";
  CHECK(cb);
  callback_ = std::move(cb);
  origin_task_runner_ = base::SingleThreadTaskRunner::GetCurrentDefault();
  stop_requested_.store(false);
  // Reset the wakeup event so Start-after-Stop doesn't see a stale
  // signaled state and exit immediately.
  stop_event_.Reset();
  capture_thread_ = std::thread(&KmsCaptureSource::CaptureThreadMain, this);
  return true;
}

void KmsCaptureSource::Stop() {
  stop_requested_.store(true);
  // Wake the capture thread out of any TimedWait-based sleep so the
  // join below returns within one vblank instead of a retry interval.
  stop_event_.Signal();
  if (capture_thread_.joinable()) {
    capture_thread_.join();
  }
  egl_.reset();
  crtc_.reset();
  callback_.Reset();
}

void KmsCaptureSource::Pause() {
  paused_.store(true);
  // Don't Signal here — pause should put the thread to sleep at its
  // next loop iteration, not wake it up. The capture loop checks
  // paused_ before its blocking call.
}

void KmsCaptureSource::Resume() {
  paused_.store(false);
  // Wake the thread out of the pause-wait so it picks up the new state.
  stop_event_.Signal();
  // ... and immediately reset so subsequent TimedWait calls block
  // again. This is racy in theory but the worst outcome is one extra
  // wakeup right after Resume, which is harmless.
  stop_event_.Reset();
}

void KmsCaptureSource::SetTargetFps(int fps) {
  if (fps <= 0) {
    fps = 1;
  }
  target_fps_.store(fps);
}

void KmsCaptureSource::CaptureThreadMain() {
  base::PlatformThread::SetName("fydeos-vnc-kmscap");

  // Lazy bring-up: don't abort if the panel is asleep at boot. Retry
  // until a CRTC shows up or Stop() is called.
  while (!stop_requested_.load()) {
    CrtcFinder finder;
    finder.SetSpec(spec_);
    crtc_ = finder.Find();
    if (crtc_) {
      break;
    }
    // Interruptible sleep: Stop() signals stop_event_ so we exit the
    // wait immediately instead of holding the joiner for up to a full
    // retry interval. TimedWait returns true on signal, false on
    // timeout — either way the next loop iteration re-checks
    // stop_requested_.
    stop_event_.TimedWait(kCrtcRetryInterval);
  }
  if (stop_requested_.load()) {
    return;
  }

  egl_ = std::make_unique<EglDisplayBuffer>(
      crtc_.get(), /*x=*/0, /*y=*/0, crtc_->width(), crtc_->height(),
      /*rotate_to_panel_orientation=*/false);

  while (!stop_requested_.load()) {
    if (paused_.load()) {
      // Block until Resume() (or Stop()) signals the event. The
      // longer the pause, the more battery this saves — GPU stays
      // idle, no readback, no memcpy. We re-check both flags after
      // every wakeup since stop_event_ is also signaled on shutdown.
      stop_event_.TimedWait(base::Seconds(1));
      stop_event_.Reset();
      continue;
    }
    const base::TimeTicks loop_start = base::TimeTicks::Now();
    EglDisplayBuffer::Result r =
        egl_->Capture(/*rotate=*/true);  // blocks on vblank inside

    // Copy the EGL-owned buffer on the capture thread: EGL reuses it for
    // the next capture, but the frame is consumed asynchronously on the
    // main thread. |owned| travels with the closure and keeps
    // frame->pixels alive until the callback returns.
    auto frame = std::make_unique<Frame>();
    frame->width = r.width;
    frame->height = r.height;
    frame->stride = r.stride;
    const size_t bytes = static_cast<size_t>(r.stride) * r.height;
    auto owned = std::make_unique<uint8_t[]>(bytes);
    std::memcpy(owned.get(), r.buffer, bytes);
    frame->pixels = owned.get();
    frame->captured_at = base::TimeTicks::Now();

    origin_task_runner_->PostTask(
        FROM_HERE,
        base::BindOnce(
            [](FrameCallback cb, std::unique_ptr<Frame> f,
               std::unique_ptr<uint8_t[]> pixels) { cb.Run(std::move(f)); },
            callback_, std::move(frame), std::move(owned)));

    // Throttle to target fps.
    const int fps = target_fps_.load();
    const base::TimeDelta target_interval = base::Hertz(fps);
    const base::TimeDelta elapsed = base::TimeTicks::Now() - loop_start;
    if (elapsed < target_interval) {
      // Interruptible throttle: stop_event_ wakes us early on Stop().
      stop_event_.TimedWait(target_interval - elapsed);
    }
  }
}

}  // namespace fydeos::vnc::capture
