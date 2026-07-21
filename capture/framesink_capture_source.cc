// Copyright 2026 The FydeOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "fydeos-vnc/capture/framesink_capture_source.h"

#include <cstring>
#include <utility>

#include <base/check.h>
#include <base/functional/bind.h>
#include <base/logging.h>
#include <base/time/time.h>
#include <mojo/public/cpp/system/buffer.h>

#include "fydeos-vnc/mojo/chrome_client.h"

namespace fydeos::vnc::capture {

FrameSinkCaptureSource::FrameSinkCaptureSource(mojo_client::ChromeClient* c,
                                               int64_t display_id)
    : chrome_(c), display_id_(display_id) {
  CHECK(chrome_);
}

FrameSinkCaptureSource::~FrameSinkCaptureSource() {
  Stop();
}

bool FrameSinkCaptureSource::Start(FrameCallback cb) {
  CHECK(cb);
  if (started_) {
    return false;
  }
  auto* fs = chrome_->frame_sink();
  if (!fs) {
    LOG(WARNING) << "FrameSink unavailable; cannot start";
    return false;
  }
  callback_ = std::move(cb);
  ::mojo::PendingRemote<::fydeos::vnc::mojom::FrameConsumer> consumer_remote;
  consumer_.Bind(consumer_remote.InitWithNewPipeAndPassReceiver());
  consumer_.set_disconnect_handler(base::BindOnce(
      &FrameSinkCaptureSource::OnDisconnected, base::Unretained(this)));
  fs->StartCapture(
      display_id_, std::move(consumer_remote),
      base::BindOnce([](::fydeos::vnc::mojom::StartCaptureResult r) {
        LOG(INFO) << "FrameSink StartCapture result="
                  << static_cast<int>(r);
      }));
  started_ = true;
  return true;
}

void FrameSinkCaptureSource::Stop() {
  if (!started_) {
    return;
  }
  started_ = false;
  if (chrome_) {
    if (auto* fs = chrome_->frame_sink()) {
      fs->StopCapture(display_id_);
    }
  }
  consumer_.reset();
  callback_.Reset();
}

void FrameSinkCaptureSource::Pause() {
  if (!started_ || paused_) {
    return;
  }
  paused_ = true;
  // Tell viz to stop pushing frames. We keep |consumer_| bound and
  // |callback_| populated so Resume can restart cheaply without going
  // through the full Mojo handshake.
  if (auto* fs = chrome_->frame_sink()) {
    fs->StopCapture(display_id_);
  }
}

void FrameSinkCaptureSource::Resume() {
  if (!started_ || !paused_) {
    return;
  }
  paused_ = false;
  if (auto* fs = chrome_->frame_sink()) {
    ::mojo::PendingRemote<::fydeos::vnc::mojom::FrameConsumer> consumer_remote;
    // The old consumer_ binding may have been reset by viz on
    // StopCapture; safer to rebind.
    consumer_.reset();
    consumer_.Bind(consumer_remote.InitWithNewPipeAndPassReceiver());
    consumer_.set_disconnect_handler(base::BindOnce(
        &FrameSinkCaptureSource::OnDisconnected, base::Unretained(this)));
    fs->StartCapture(
        display_id_, std::move(consumer_remote),
        base::BindOnce([](::fydeos::vnc::mojom::StartCaptureResult r) {
          LOG(INFO) << "FrameSink Resume StartCapture result="
                    << static_cast<int>(r);
        }));
  }
}

bool FrameSinkCaptureSource::SwitchDisplay(int64_t new_display_id) {
  if (!started_) {
    return false;
  }
  auto* fs = chrome_->frame_sink();
  if (!fs) {
    return false;
  }
  LOG(INFO) << "FrameSink switching display " << display_id_ << " -> "
            << new_display_id;
  fs->StopCapture(display_id_);
  display_id_ = new_display_id;
  if (paused_) {
    // Resume() will StartCapture with the new id; nothing to stream now.
    return true;
  }
  // Chrome tears down the old VizConsumer on StopCapture; rebind a fresh
  // pipe like Resume() does.
  ::mojo::PendingRemote<::fydeos::vnc::mojom::FrameConsumer> consumer_remote;
  consumer_.reset();
  consumer_.Bind(consumer_remote.InitWithNewPipeAndPassReceiver());
  consumer_.set_disconnect_handler(base::BindOnce(
      &FrameSinkCaptureSource::OnDisconnected, base::Unretained(this)));
  fs->StartCapture(
      display_id_, std::move(consumer_remote),
      base::BindOnce(&FrameSinkCaptureSource::OnSwitchStartResult,
                     weak_factory_.GetWeakPtr()));
  return true;
}

void FrameSinkCaptureSource::OnSwitchStartResult(
    ::fydeos::vnc::mojom::StartCaptureResult result) {
  if (result == ::fydeos::vnc::mojom::StartCaptureResult::kOk) {
    LOG(INFO) << "FrameSink switch StartCapture ok display=" << display_id_;
    return;
  }
  LOG(WARNING) << "FrameSink switch StartCapture failed result="
               << static_cast<int>(result) << "; dropping stream";
  StreamGone();  // may delete |this|; nothing after this call
}

void FrameSinkCaptureSource::SetTargetFps(int fps) {
  if (fps <= 0) {
    fps = 1;
  }
  target_fps_.store(fps);
  // Advisory only: the Chrome-side capturer caps the rate (30 fps) and
  // viz emits frames damage-driven below that.
}

void FrameSinkCaptureSource::OnFrameCaptured(
    ::mojo::ScopedSharedBufferHandle buffer,
    uint64_t size_bytes,
    uint32_t width,
    uint32_t height,
    uint32_t stride,
    int64_t timestamp_us,
    std::vector<::fydeos::vnc::mojom::DamageRectPtr> damage) {
  if (!started_ || !callback_) {
    return;
  }
  ::mojo::ScopedSharedBufferMapping mapping = buffer->Map(size_bytes);
  if (!mapping) {
    LOG(WARNING) << "FrameSink: shmem map failed (need " << size_bytes
                 << " bytes)";
    return;
  }

  auto frame = std::make_unique<Frame>();
  frame->width = width;
  frame->height = height;
  frame->stride = stride;
  // Borrow the shmem mapping directly — no intermediate copy. Frame's
  // contract says |pixels| is only valid until the callback returns,
  // and callback_.Run is synchronous here; |mapping| outlives the call.
  frame->pixels = mapping.get();
  frame->captured_at = base::TimeTicks::Now();
  for (const auto& d : damage) {
    frame->damage.push_back({d->x, d->y, d->w, d->h});
  }
  callback_.Run(std::move(frame));
}

void FrameSinkCaptureSource::OnStopped() {
  LOG(INFO) << "FrameSink stream stopped by Chrome";
  StreamGone();
}

void FrameSinkCaptureSource::OnDisconnected() {
  LOG(INFO) << "FrameSink consumer pipe disconnected (Chrome gone?)";
  StreamGone();
}

void FrameSinkCaptureSource::StreamGone() {
  if (!started_) {
    return;
  }
  // Clean up before signaling upstream: the null-frame callback makes the
  // selector destroy this object (GoBackToKms resets |framesink_|), so
  // touching any member after cb.Run would be a use-after-free. No
  // StopCapture round-trip either — the stream/pipe is already gone.
  started_ = false;
  consumer_.reset();
  auto cb = std::move(callback_);
  callback_.Reset();
  if (cb) {
    cb.Run(nullptr);  // may delete |this|; must be the last statement
  }
}

}  // namespace fydeos::vnc::capture
