// Copyright 2026 The FydeOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FYDEOS_VNC_CAPTURE_FRAMESINK_CAPTURE_SOURCE_H_
#define FYDEOS_VNC_CAPTURE_FRAMESINK_CAPTURE_SOURCE_H_

#include <atomic>
#include <memory>
#include <vector>

#include <base/memory/raw_ptr.h>
#include <base/memory/weak_ptr.h>
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
  // |display_id| selects which display Chrome captures; 0 = primary.
  FrameSinkCaptureSource(mojo_client::ChromeClient* chrome,
                         int64_t display_id);
  ~FrameSinkCaptureSource() override;

  // CaptureSource:
  bool Start(FrameCallback cb) override;
  void Stop() override;
  void Pause() override;
  void Resume() override;
  void SetTargetFps(int fps) override;

  int64_t display_id() const { return display_id_; }

  // Retargets the running stream to |new_display_id| in place (StopCapture
  // old id, rebind consumer, StartCapture new id). Returns false if not
  // started or Chrome is unreachable. If Chrome later reports the async
  // StartCapture failed, the stream is torn down via StreamGone() and the
  // selector falls back to KMS (then auto-recovers on primary).
  bool SwitchDisplay(int64_t new_display_id);

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
  // Fires when the consumer pipe breaks without a prior OnStopped() —
  // Chrome exits on logout without tearing captures down gracefully.
  void OnDisconnected();
  // Shared teardown for OnStopped/OnDisconnected. Signals upstream with a
  // null frame as its final action; |this| may be destroyed by that call.
  void StreamGone();
  // Async reply for the StartCapture issued by SwitchDisplay. Bound via
  // WeakPtr: the selector may destroy this source (GoBackToKms) before
  // the reply arrives.
  void OnSwitchStartResult(::fydeos::vnc::mojom::StartCaptureResult result);
  base::raw_ptr<mojo_client::ChromeClient> chrome_;
  int64_t display_id_;
  ::mojo::Receiver<::fydeos::vnc::mojom::FrameConsumer> consumer_{this};
  FrameCallback callback_;
  std::atomic<int> target_fps_{30};
  bool started_ = false;
  bool paused_ = false;
  base::WeakPtrFactory<FrameSinkCaptureSource> weak_factory_{this};
};

}  // namespace fydeos::vnc::capture

#endif  // FYDEOS_VNC_CAPTURE_FRAMESINK_CAPTURE_SOURCE_H_
