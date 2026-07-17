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
  StartKms();
  uinput_ = std::make_unique<input::UinputInputSink>(config_.source_width,
                                                      config_.source_height);
  state_ = State::kKmsActive;

  // Try to enter FrameSink immediately; if Chrome isn't reachable yet
  // (boot ordering) or goes away later (logout restarts it), the
  // reconnect timer keeps retrying from the KMS state.
  TryEnterFrameSink();
  if (state_ == State::kKmsActive) {
    reconnect_timer_.Start(
        FROM_HERE, config_.reconnect_interval,
        base::BindRepeating(&CaptureSourceSelector::OnReconnectTick,
                            base::Unretained(this)));
  }
  return true;
}

void CaptureSourceSelector::StartKms() {
  kms_ = std::make_unique<capture::KmsCaptureSource>(config_.display_spec);
  kms_->SetTargetFps(config_.pre_login_fps);
  kms_->Start(base::BindRepeating(
      &CaptureSourceSelector::OnFrameFromActive, base::Unretained(this)));
  if (paused_) {
    kms_->Pause();
  }
}

void CaptureSourceSelector::Stop() {
  probe_timeout_timer_.Stop();
  reconnect_timer_.Stop();
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
  paused_ = true;
  if (kms_) {
    kms_->Pause();
  }
}

void CaptureSourceSelector::Resume() {
  paused_ = false;
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
  if (!chrome_ || state_ != State::kKmsActive) {
    return;
  }
  if (!chrome_->EnsureConnected()) {
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
  probe_timeout_timer_.Stop();
  if (framesink_) {
    framesink_->Stop();
    framesink_.reset();
  }
  ash_.reset();
  state_ = State::kKmsActive;
  // KMS was torn down when FrameSink went active; bring it back so the
  // client keeps getting frames (e.g. the login screen after logout).
  if (!kms_) {
    StartKms();
  }
  // Chrome will come back (login screen / next session); keep retrying
  // FrameSink entry so we regain the cheap capture path.
  if (!reconnect_timer_.IsRunning()) {
    reconnect_timer_.Start(
        FROM_HERE, config_.reconnect_interval,
        base::BindRepeating(&CaptureSourceSelector::OnReconnectTick,
                            base::Unretained(this)));
  }
}

void CaptureSourceSelector::OnReconnectTick() {
  if (state_ != State::kKmsActive) {
    reconnect_timer_.Stop();
    return;
  }
  TryEnterFrameSink();
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
    reconnect_timer_.Stop();
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
