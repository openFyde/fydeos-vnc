// Copyright 2026 The FydeOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FYDEOS_VNC_DAEMON_CAPTURE_SOURCE_SELECTOR_H_
#define FYDEOS_VNC_DAEMON_CAPTURE_SOURCE_SELECTOR_H_

#include <memory>
#include <string>
#include <vector>

#include <base/functional/callback.h>
#include <base/memory/raw_ptr.h>
#include <base/timer/timer.h>
#include <mojo/public/cpp/bindings/receiver.h>

#include "fydeos-vnc/capture/capture_source.h"
#include "fydeos-vnc/capture/framesink_capture_source.h"
#include "fydeos-vnc/capture/kms_capture_source.h"
#include "fydeos-vnc/input/ash_input_sink.h"
#include "fydeos-vnc/input/input_sink.h"
#include "fydeos-vnc/input/uinput_sink.h"

namespace fydeos::vnc::mojo_client {
class ChromeClient;
}

namespace fydeos::vnc::daemon {

// Decides between KMS+uinput and FrameSink+Ash sinks based on whether
// Chrome's Mojo bootstrap is reachable. State machine:
//
//   KMS_ACTIVE                  -- pre-login / Chrome down
//     -> PROBING_FRAMESINK      -- Mojo bootstrap call sent
//     -> FRAMESINK_ACTIVE       -- first FrameSink frame received
//     -> KMS_ACTIVE             -- pipe disconnect / OnStopped()
//
// One instance per Pipeline. Owns the active capture + input sink pair.
// Forwards captured frames to the daemon-supplied callback. Hides the
// switch from upstream — same FrameCallback shape regardless of source.
class CaptureSourceSelector : public ::fydeos::vnc::mojom::DisplayObserver {
 public:
  enum class State { kKmsActive, kProbingFrameSink, kFrameSinkActive };

  enum class SwitchResult {
    kOk,
    kNotInFrameSinkMode,   // switching needs a live Chrome capture path
    kUnknownDisplay,       // id not in the Chrome-pushed display list
    kChromeUnavailable,    // FrameSink interface gone mid-request
  };

  struct DisplaySnapshot {
    int64_t id;
    std::string name;
    uint32_t width;
    uint32_t height;
    bool is_primary;
  };

  struct Config {
    capture::CrtcFinder::Spec display_spec =
        capture::CrtcFinder::Spec::kAnyDisplay;
    uint32_t source_width;
    uint32_t source_height;
    int pre_login_fps = 5;
    int post_login_fps = 30;
    // 5s without any FrameSink frame after StartCapture is treated as
    // failure and we fall back to KMS.
    base::TimeDelta probe_timeout = base::Seconds(5);
    // While on KMS, retry the Chrome bootstrap + FrameSink entry at this
    // cadence. Covers Chrome coming up after the daemon at boot and
    // Chrome restarting on logout.
    base::TimeDelta reconnect_interval = base::Seconds(5);
    // Fired whenever the captured display or the display list changes
    // (switch, hotplug, KMS fallback). Pipeline forwards it to the D-Bus
    // StatusChanged signal.
    base::RepeatingClosure display_state_changed;
  };

  CaptureSourceSelector(Config config,
                         mojo_client::ChromeClient* chrome,
                         capture::CaptureSource::FrameCallback frame_cb);
  CaptureSourceSelector(const CaptureSourceSelector&) = delete;
  CaptureSourceSelector& operator=(const CaptureSourceSelector&) = delete;
  ~CaptureSourceSelector() override;

  bool Start();
  void Stop();
  // Suspend GPU/CPU work on both underlying sources without
  // destroying them. Used by Pipeline so toggle-off doesn't trigger
  // the KmsCaptureSource teardown that deadlocks behind Mesa/EGL.
  void Pause();
  void Resume();

  State state() const { return state_; }
  input::InputSink* input_sink();  // returns the currently-active sink

  // Retargets the FrameSink capture to |display_id| (0 = primary). Only
  // valid in FRAMESINK_ACTIVE; the KMS path stays primary-only.
  SwitchResult SwitchToDisplay(int64_t display_id);

  // Pipeline-fed: whether at least one authenticated VNC client is
  // connected. Cached and forwarded to Chrome (drives the floating
  // display-switcher visibility); re-sent on every FrameSink (re-)entry
  // so Chrome restarts pick the current value back up.
  void SetClientPresence(bool present);
  int64_t current_display_id() const { return current_display_id_; }
  // Snapshot of the Chrome-pushed display list (empty on KMS).
  std::vector<DisplaySnapshot> GetDisplays() const;

  // mojom::DisplayObserver:
  void OnDisplaysChanged(
      std::vector<::fydeos::vnc::mojom::DisplayInfoPtr> displays) override;

 private:
  void OnFrameFromActive(std::unique_ptr<capture::Frame> frame);
  void TryEnterFrameSink();
  void GoBackToKms();
  void StartKms();
  void OnReconnectTick();

  const Config config_;
  base::raw_ptr<mojo_client::ChromeClient> chrome_;
  capture::CaptureSource::FrameCallback frame_cb_;

  State state_ = State::kKmsActive;
  std::unique_ptr<capture::KmsCaptureSource> kms_;
  std::unique_ptr<capture::FrameSinkCaptureSource> framesink_;
  std::unique_ptr<input::UinputInputSink> uinput_;
  std::unique_ptr<input::AshInputSink> ash_;
  base::OneShotTimer probe_timeout_timer_;
  base::RepeatingTimer reconnect_timer_;
  // Mirrors Pipeline's Pause/Resume so a KMS source recreated after a
  // FrameSink fallback starts in the right idle state.
  bool paused_ = false;
  // Display currently captured in FrameSink mode. 0 = primary; reset to 0
  // on every KMS fallback so Chrome re-entry always lands on primary.
  int64_t current_display_id_ = 0;
  bool clients_present_ = false;
  // Chrome-pushed display list; authoritative while FrameSink is up.
  std::vector<::fydeos::vnc::mojom::DisplayInfoPtr> displays_;
  ::mojo::Receiver<::fydeos::vnc::mojom::DisplayObserver>
      display_observer_receiver_{this};
};

const char* StateToString(CaptureSourceSelector::State s);

}  // namespace fydeos::vnc::daemon

#endif  // FYDEOS_VNC_DAEMON_CAPTURE_SOURCE_SELECTOR_H_
