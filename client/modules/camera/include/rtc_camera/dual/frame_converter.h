#pragma once

#include "rtc_camera/dual/async_image_source.h"

#include <stddef.h>
#include <stdint.h>

#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace rtc_camera {
namespace dual {

class DualUyvyToI420StitchCudaConverter;

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

  bool ConvertToI420(const ImageFrame& frame,
                     ConvertedI420Frame* output,
                     std::string* error_message);
  bool EnqueueToI420(const ImageFrame& frame,
                     std::string* error_message);
  bool DequeueI420(ConvertedI420Frame* output,
                   std::string* error_message);
  void DiscardPending();
  size_t pending_frames() const;

 private:
  bool EnsureConverter(const ImageFrame& frame,
                       std::string* error_message);
  bool ConvertNv12DualToI420(const ImageFrame& frame,
                             ConvertedI420Frame* output,
                             std::string* error_message);

  std::unique_ptr<DualUyvyToI420StitchCudaConverter> converter_;
  size_t left_width_ = 0;
  size_t left_height_ = 0;
  size_t left_stride_bytes_ = 0;
  size_t right_width_ = 0;
  size_t right_height_ = 0;
  size_t right_stride_bytes_ = 0;
  std::deque<ImageFrame> pending_cuda_frames_;

  // NV12 CPU 转换缓冲区。
  std::vector<uint8_t> left_y_;
  std::vector<uint8_t> left_u_;
  std::vector<uint8_t> left_v_;
  std::vector<uint8_t> right_y_;
  std::vector<uint8_t> right_u_;
  std::vector<uint8_t> right_v_;
  std::vector<uint8_t> stitched_i420_;
};

}  // 命名空间 dual
}  // 命名空间 rtc_camera
