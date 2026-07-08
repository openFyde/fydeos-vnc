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

namespace {
constexpr int64_t kPrimaryDisplay = 0;
}  // namespace

FrameSinkCaptureSource::FrameSinkCaptureSource(mojo_client::ChromeClient* c)
    : chrome_(c) {
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
  fs->StartCapture(
      kPrimaryDisplay, std::move(consumer_remote),
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
      fs->StopCapture(kPrimaryDisplay);
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
    fs->StopCapture(kPrimaryDisplay);
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
    fs->StartCapture(
        kPrimaryDisplay, std::move(consumer_remote),
        base::BindOnce([](::fydeos::vnc::mojom::StartCaptureResult r) {
          LOG(INFO) << "FrameSink Resume StartCapture result="
                    << static_cast<int>(r);
        }));
  }
}

void FrameSinkCaptureSource::SetTargetFps(int fps) {
  if (fps <= 0) {
    fps = 1;
  }
  target_fps_.store(fps);
  // M7a: target fps is advisory. Chrome side stub emits at fixed 30fps.
  // M7b will plumb this to viz capturer's SetMinCapturePeriod.
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
  if (callback_) {
    callback_.Run(nullptr);  // signal upstream that source is gone
  }
  Stop();
}

}  // namespace fydeos::vnc::capture
