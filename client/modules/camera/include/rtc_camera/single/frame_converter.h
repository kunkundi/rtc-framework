#pragma once

#include "rtc_camera/single/async_image_source.h"

#include <stddef.h>
#include <stdint.h>

#include <memory>
#include <string>
#include <vector>

namespace rtc_camera {
namespace single {

class UyvyToI420CudaConverter;

struct ConvertedCameraFrame {
  const uint8_t* data = nullptr;
  size_t data_size = 0;
  size_t width = 0;
  size_t height = 0;
  size_t stride_y = 0;
  size_t stride_u = 0;
  size_t stride_v = 0;
};

class CameraFrameConverter {
 public:
  CameraFrameConverter();
  ~CameraFrameConverter();

  CameraFrameConverter(const CameraFrameConverter&) = delete;
  CameraFrameConverter& operator=(const CameraFrameConverter&) = delete;

  bool ConvertToI420(const CameraFrame& frame,
                     ConvertedCameraFrame* output,
                     std::string* error_message);

 private:
  bool ConvertNv12(const CameraFrame& frame,
                   ConvertedCameraFrame* output,
                   std::string* error_message);
  bool EnsureCudaConverter(const CameraFrame& frame,
                           std::string* error_message);

  std::unique_ptr<UyvyToI420CudaConverter> cuda_converter_;
  uint32_t cuda_pixel_format_ = 0;
  size_t cuda_width_ = 0;
  size_t cuda_height_ = 0;
  size_t cuda_stride_bytes_ = 0;
  std::vector<uint8_t> nv12_i420_;
};

}  // 命名空间 single
}  // 命名空间 rtc_camera
