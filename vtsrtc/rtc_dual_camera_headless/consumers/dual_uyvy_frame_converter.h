#pragma once

#include "rtc_dual_camera/dual_camera_async_image_source.h"

#include <stddef.h>
#include <stdint.h>

#include <memory>
#include <string>

class DualUyvyToI420StitchCudaConverter;

namespace rtc_camera_headless {

struct ConvertedI420Frame {
  const uint8_t* data = nullptr;
  size_t data_size = 0;
  size_t width = 0;
  size_t height = 0;
  size_t stride_y = 0;
  size_t stride_u = 0;
  size_t stride_v = 0;
};

class DualUyvyFrameConverter {
 public:
  DualUyvyFrameConverter();
  ~DualUyvyFrameConverter();

  DualUyvyFrameConverter(const DualUyvyFrameConverter&) = delete;
  DualUyvyFrameConverter& operator=(const DualUyvyFrameConverter&) = delete;

  bool ConvertToI420(const rtc_dual_camera::ImageFrame& frame,
                     ConvertedI420Frame* output,
                     std::string* error_message);

 private:
  bool EnsureConverter(const rtc_dual_camera::ImageFrame& frame,
                       std::string* error_message);

  std::unique_ptr<DualUyvyToI420StitchCudaConverter> converter_;
  size_t left_width_ = 0;
  size_t left_height_ = 0;
  size_t left_stride_bytes_ = 0;
  size_t right_width_ = 0;
  size_t right_height_ = 0;
  size_t right_stride_bytes_ = 0;
};

}  // namespace rtc_camera_headless
