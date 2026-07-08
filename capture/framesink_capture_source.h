// Copyright 2026 The FydeOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FYDEOS_VNC_CAPTURE_FRAMESINK_CAPTURE_SOURCE_H_
#define FYDEOS_VNC_CAPTURE_FRAMESINK_CAPTURE_SOURCE_H_

#include <atomic>
#include <memory>
#include <vector>

#include <base/memory/raw_ptr.h>
#include <mojo/public/cpp/bindings/pending_remote.h>
#include <mojo/public/cpp/bindings/receiver.h>

#include <mojo/public/cpp/system/buffer.h>

#include "fydeos-vnc/capture/capture_source.h"
#include "mojo/fydeos_vnc.mojom.h"

namespace fydeos::vnc::mojo_client {
class ChromeClient;
}

namespace fydeos::vnc::capture {

// Post-login capture path. Talks to Chrome's FrameSinkCaptureService over
// Mojo; viz frames arrive in shared memory and are handed to the consumer
// callback as borrowed Frame pixels.
class FrameSinkCaptureSource
    : public CaptureSource,
      public ::fydeos::vnc::mojom::FrameConsumer {
 public:
  explicit FrameSinkCaptureSource(mojo_client::ChromeClient* chrome);
  ~FrameSinkCaptureSource() override;

  // CaptureSource:
  bool Start(FrameCallback cb) override;
  void Stop() override;
  void Pause() override;
  void Resume() override;
  void SetTargetFps(int fps) override;

  // mojom::FrameConsumer:
  void OnFrameCaptured(
      ::mojo::ScopedSharedBufferHandle buffer,
      uint64_t size_bytes,
      uint32_t width,
      uint32_t height,
      uint32_t stride,
      int64_t timestamp_us,
      std::vector<::fydeos::vnc::mojom::DamageRectPtr> damage) override;
  void OnStopped() override;

 private:
  base::raw_ptr<mojo_client::ChromeClient> chrome_;
  ::mojo::Receiver<::fydeos::vnc::mojom::FrameConsumer> consumer_{this};
  FrameCallback callback_;
  std::atomic<int> target_fps_{30};
  bool started_ = false;
  bool paused_ = false;
};

}  // namespace fydeos::vnc::capture

#endif  // FYDEOS_VNC_CAPTURE_FRAMESINK_CAPTURE_SOURCE_H_
