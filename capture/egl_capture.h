// Copyright 2020 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FYDEOS_VNC_CAPTURE_EGL_CAPTURE_H_
#define FYDEOS_VNC_CAPTURE_EGL_CAPTURE_H_

#include <gbm.h>
#include <stdint.h>

#include <memory>
#include <vector>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <EGL/eglplatform.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include "fydeos-vnc/capture/ptr_util.h"

namespace fydeos::vnc::capture {

class Crtc;

constexpr int kBytesPerPixel = 4;
static_assert(kBytesPerPixel == sizeof(uint32_t),
              "kBytesPerPixel must be 4 bytes");

// Low-level EGL/dma-buf importer + glReadPixels readback. Owned by
// KmsCaptureSource; not exposed to daemon code directly.
class EglDisplayBuffer {
 public:
  struct Result {
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    void* buffer;
  };

  EglDisplayBuffer(const Crtc* crtc,
                   uint32_t x,
                   uint32_t y,
                   uint32_t width,
                   uint32_t height,
                   bool rotate_to_panel_orientation);
  EglDisplayBuffer(const EglDisplayBuffer&) = delete;
  EglDisplayBuffer& operator=(const EglDisplayBuffer&) = delete;
  ~EglDisplayBuffer();
  // Captures a screenshot from the specified CRTC.
  Result Capture(bool rotate);

  // Rotates the capture result by 90 degree clockwise, and updates
  // the geometric parameters (width, height, and stride).
  static void Rotate(Result& result, std::vector<uint32_t>& buffer);

 private:
  // Sets the UV coordinates uniform for a crop rectangle with respect to
  // |src_width| and |src_height|.
  void SetUVRect(float crop_x,
                 float crop_y,
                 float crop_width,
                 float crop_height,
                 float src_width,
                 float src_height);

  const Crtc& crtc_;
  const uint32_t x_;
  const uint32_t y_;
  const uint32_t width_;
  const uint32_t height_;
  const ScopedGbmDevicePtr device_;
  const EGLDisplay display_;
  const bool rotate_to_panel_orientation_;

  GLint uvs_uniform_location_;
  GLuint input_texture_;
  GLuint output_texture_;
  unsigned int fbo_;
  EGLContext ctx_;
  PFNEGLCREATEIMAGEKHRPROC createImageKHR_;
  PFNEGLDESTROYIMAGEKHRPROC destroyImageKHR_;
  PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES_;
  bool import_modifiers_exist_;
  std::vector<uint32_t> buffer_;
  // Capture() will be called repeatedly in kmsvnc, thus a reusable buffer
  // is allocated for rotation.
  std::vector<uint32_t> rotated_;
};

}  // namespace fydeos::vnc::capture

#endif  // FYDEOS_VNC_CAPTURE_EGL_CAPTURE_H_
