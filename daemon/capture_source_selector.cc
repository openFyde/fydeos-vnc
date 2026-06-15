// Copyright 2026 The FydeOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "fydeos-vnc/daemon/capture_source_selector.h"

#include <utility>

#include <base/check.h>
#include <base/functional/bind.h>
#include <base/logging.h>

#include "fydeos-vnc/mojo/chrome_client.h"

namespace fydeos::vnc::daemon {

const char* StateToString(CaptureSourceSelector::State s) {
  switch (s) {
    case CaptureSourceSelector::State::kKmsActive:
      return "KMS_ACTIVE";
    case CaptureSourceSelector::State::kProbingFrameSink:
      return "PROBING_FRAMESINK";
    case CaptureSourceSelector::State::kFrameSinkActive:
      return "FRAMESINK_ACTIVE";
  }
  return "?";
}

CaptureSourceSelector::CaptureSourceSelector(
    Config config,
    mojo_client::ChromeClient* chrome,
    capture::CaptureSource::FrameCallback frame_cb)
    : config_(std::move(config)),
      chrome_(chrome),
      frame_cb_(std::move(frame_cb)) {
  CHECK(frame_cb_);
}

CaptureSourceSelector::~CaptureSourceSelector() {
  Stop();
}

bool CaptureSourceSelector::Start() {
  // Always bring KMS up first — it's the fallback path. Even when we
  // succeed switching to FrameSink, we keep KMS torn down to avoid two
  // CRTC consumers fighting for DRM master. The selector recreates KMS
  // when going back.
  kms_ = std::make_unique<capture::KmsCaptureSource>(config_.display_spec);
  kms_->SetTargetFps(config_.pre_login_fps);
  kms_->Start(base::BindRepeating(
      &CaptureSourceSelector::OnFrameFromActive, base::Unretained(this)));
  uinput_ = std::make_unique<input::UinputInputSink>(config_.source_width,
                                                      config_.source_height);
  state_ = State::kKmsActive;

  // Try to enter FrameSink immediately. If Chrome isn't reachable, we stay
  // on KMS and could retry later (M7 leaves the retry as a TODO — the
  // primary use case is "Chrome up at daemon-enable time").
  TryEnterFrameSink();
  return true;
}

void CaptureSourceSelector::Stop() {
  probe_timeout_timer_.Stop();
  if (framesink_) {
    framesink_->Stop();
    framesink_.reset();
  }
  if (kms_) {
    kms_->Stop();
    kms_.reset();
  }
  ash_.reset();
  uinput_.reset();
  state_ = State::kKmsActive;
}

void CaptureSourceSelector::Pause() {
  // Only KMS is suspended. FrameSink is self-throttling — viz only
  // emits frames when there's damage, so an "idle" framesink already
  // burns close to zero CPU. Pausing the framesink path would also
  // stop refreshing the libvncserver framebuffer, leaving the next
  // client that connects looking at stale (or all-zero) content.
  if (kms_) {
    kms_->Pause();
  }
}

void CaptureSourceSelector::Resume() {
  if (kms_) {
    kms_->Resume();
  }
}

input::InputSink* CaptureSourceSelector::input_sink() {
  if (state_ == State::kFrameSinkActive && ash_) {
    return ash_.get();
  }
  return uinput_.get();
}

void CaptureSourceSelector::TryEnterFrameSink() {
  if (!chrome_) {
    return;
  }
  auto* fs = chrome_->frame_sink();
  if (!fs) {
    LOG(INFO) << "Selector: Chrome FrameSink not available; staying on KMS";
    return;
  }
  state_ = State::kProbingFrameSink;
  LOG(INFO) << "Selector → " << StateToString(state_);
  framesink_ = std::make_unique<capture::FrameSinkCaptureSource>(chrome_);
  framesink_->SetTargetFps(config_.post_login_fps);
  if (!framesink_->Start(base::BindRepeating(
          &CaptureSourceSelector::OnFrameFromActive,
          base::Unretained(this)))) {
    framesink_.reset();
    state_ = State::kKmsActive;
    return;
  }
  ash_ = std::make_unique<input::AshInputSink>(chrome_);
  probe_timeout_timer_.Start(
      FROM_HERE, config_.probe_timeout,
      base::BindOnce(&CaptureSourceSelector::GoBackToKms,
                     base::Unretained(this)));
}

void CaptureSourceSelector::GoBackToKms() {
  if (state_ == State::kKmsActive) {
    return;
  }
  LOG(INFO) << "Selector: falling back to KMS_ACTIVE (FrameSink unhealthy)";
  if (framesink_) {
    framesink_->Stop();
    framesink_.reset();
  }
  ash_.reset();
  state_ = State::kKmsActive;
}

void CaptureSourceSelector::OnFrameFromActive(
    std::unique_ptr<capture::Frame> frame) {
  if (!frame) {
    // Source signaled stop. If it was FrameSink, fall back to KMS.
    if (state_ != State::kKmsActive) {
      GoBackToKms();
    }
    return;
  }
  if (state_ == State::kProbingFrameSink) {
    // The first frame from FrameSink confirms the path is live.
    probe_timeout_timer_.Stop();
    state_ = State::kFrameSinkActive;
    LOG(INFO) << "Selector → " << StateToString(state_)
              << " (got first frame " << frame->width << "x"
              << frame->height << ")";
    // Stop KMS — we no longer need pre-login readback.
    if (kms_) {
      kms_->Stop();
      kms_.reset();
    }
  }
  frame_cb_.Run(std::move(frame));
}

}  // namespace fydeos::vnc::daemon
