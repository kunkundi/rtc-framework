#include "rtc_camera/dual/frame_converter.h"

#include "internal/uyvy_to_i420_stitch_cuda.h"

#include <linux/videodev2.h>
#include <memory>

namespace rtc_camera {
namespace dual {
namespace {

bool IsConvertiblePixelFormat(uint32_t pixel_format) {
  return pixel_format == V4L2_PIX_FMT_UYVY ||
         pixel_format == V4L2_PIX_FMT_YUYV ||
         pixel_format == V4L2_PIX_FMT_NV12;
}

}  // 匿名命名空间

DualUyvyFrameConverter::DualUyvyFrameConverter() = default;
DualUyvyFrameConverter::~DualUyvyFrameConverter() = default;

bool DualUyvyFrameConverter::EnsureConverter(
    const ImageFrame& frame,
    std::string* error_message) {
  if (frame.format != ImagePixelFormat::kDualUyvy) {
    if (error_message) {
      *error_message = "frame is not a raw dual-camera frame";
    }
    return false;
  }
  if (frame.empty() || frame.left_width == 0 || frame.left_height == 0 ||
      frame.right_width == 0 || frame.right_height == 0 ||
      frame.left_stride_bytes == 0 || frame.right_stride_bytes == 0 ||
      !IsConvertiblePixelFormat(frame.left_pixel_format) ||
      !IsConvertiblePixelFormat(frame.right_pixel_format)) {
    if (error_message) {
      *error_message = "invalid raw dual-camera frame";
    }
    return false;
  }

  if (converter_ && left_pixel_format_ == frame.left_pixel_format &&
      left_width_ == frame.left_width &&
      left_height_ == frame.left_height &&
      left_stride_bytes_ == frame.left_stride_bytes &&
      right_pixel_format_ == frame.right_pixel_format &&
      right_width_ == frame.right_width &&
      right_height_ == frame.right_height &&
      right_stride_bytes_ == frame.right_stride_bytes) {
    return true;
  }

  DiscardPending();

  std::unique_ptr<DualUyvyToI420StitchCudaConverter> converter(
      new DualUyvyToI420StitchCudaConverter());
  if (!converter->Init(frame.left_width, frame.left_height,
                       frame.left_stride_bytes,
                       frame.left_pixel_format == V4L2_PIX_FMT_YUYV,
                       frame.left_pixel_format == V4L2_PIX_FMT_NV12,
                       frame.right_width, frame.right_height,
                       frame.right_stride_bytes,
                       frame.right_pixel_format == V4L2_PIX_FMT_YUYV,
                       frame.right_pixel_format == V4L2_PIX_FMT_NV12,
                       error_message)) {
    return false;
  }

  converter_ = std::move(converter);
  left_pixel_format_ = frame.left_pixel_format;
  left_width_ = frame.left_width;
  left_height_ = frame.left_height;
  left_stride_bytes_ = frame.left_stride_bytes;
  right_pixel_format_ = frame.right_pixel_format;
  right_width_ = frame.right_width;
  right_height_ = frame.right_height;
  right_stride_bytes_ = frame.right_stride_bytes;
  return true;
}

bool DualUyvyFrameConverter::ConvertToI420(
    const ImageFrame& frame,
    ConvertedI420Frame* output,
    std::string* error_message) {
  if (!output) {
    if (error_message) {
      *error_message = "null converted frame output";
    }
    return false;
  }
  *output = ConvertedI420Frame();
  DiscardPending();

  if (frame.format == ImagePixelFormat::kI420) {
    if (frame.empty() || frame.stride_y == 0 || frame.stride_u == 0 ||
        frame.stride_v == 0) {
      if (error_message) {
        *error_message = "invalid I420 frame";
      }
      return false;
    }
    output->data = frame.data;
    output->data_size = frame.data_size;
    output->width = frame.width;
    output->height = frame.height;
    output->stride_y = frame.stride_y;
    output->stride_u = frame.stride_u;
    output->stride_v = frame.stride_v;
    return true;
  }

  // 原始双目格式统一使用 CUDA 完成转换和拼接。
  if (!EnsureConverter(frame, error_message)) {
    return false;
  }

  const uint8_t* converted_data = nullptr;
  size_t converted_size = 0;
  if (!converter_->Convert(frame.left_data, frame.left_data_size,
                           frame.right_data, frame.right_data_size,
                           &converted_data, &converted_size,
                           error_message)) {
    return false;
  }

  output->data = converted_data;
  output->data_size = converted_size;
  output->width = converter_->output_width();
  output->height = converter_->output_height();
  output->stride_y = converter_->y_stride();
  output->stride_u = converter_->u_stride();
  output->stride_v = converter_->v_stride();
  return true;
}

bool DualUyvyFrameConverter::EnqueueToI420(
    const ImageFrame& frame,
    std::string* error_message) {
  if (!EnsureConverter(frame, error_message) ||
      !converter_->Enqueue(frame.left_data, frame.left_data_size,
                           frame.right_data, frame.right_data_size,
                           error_message)) {
    return false;
  }
  pending_cuda_frames_.push_back(frame);
  return true;
}

bool DualUyvyFrameConverter::DequeueI420(
    ConvertedI420Frame* output,
    std::string* error_message) {
  if (!output) {
    if (error_message) {
      *error_message = "null converted frame output";
    }
    return false;
  }
  *output = ConvertedI420Frame();
  if (!converter_ || pending_cuda_frames_.empty()) {
    if (error_message) {
      *error_message = "No asynchronous stereo frame is pending";
    }
    return false;
  }

  const uint8_t* converted_data = nullptr;
  size_t converted_size = 0;
  if (!converter_->Dequeue(&converted_data, &converted_size, error_message)) {
    return false;
  }
  pending_cuda_frames_.pop_front();
  output->data = converted_data;
  output->data_size = converted_size;
  output->width = converter_->output_width();
  output->height = converter_->output_height();
  output->stride_y = converter_->y_stride();
  output->stride_u = converter_->u_stride();
  output->stride_v = converter_->v_stride();
  return true;
}

void DualUyvyFrameConverter::DiscardPending() {
  if (converter_) {
    converter_->DiscardPending();
  }
  pending_cuda_frames_.clear();
}

size_t DualUyvyFrameConverter::pending_frames() const {
  return pending_cuda_frames_.size();
}

}  // 命名空间 dual
}  // 命名空间 rtc_camera
