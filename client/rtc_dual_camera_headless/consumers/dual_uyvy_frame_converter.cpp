#include "dual_uyvy_frame_converter.h"

#include "dual_uyvy_to_i420_stitch_cuda.h"

#include <memory>

namespace rtc_camera_headless {

DualUyvyFrameConverter::DualUyvyFrameConverter() = default;
DualUyvyFrameConverter::~DualUyvyFrameConverter() = default;

bool DualUyvyFrameConverter::EnsureConverter(
    const rtc_dual_camera::ImageFrame& frame,
    std::string* error_message) {
  if (frame.format != rtc_dual_camera::ImagePixelFormat::kDualUyvy) {
    if (error_message) {
      *error_message = "frame is not a dual UYVY frame";
    }
    return false;
  }
  if (frame.empty() || frame.left_width == 0 || frame.left_height == 0 ||
      frame.right_width == 0 || frame.right_height == 0 ||
      frame.left_stride_bytes == 0 || frame.right_stride_bytes == 0) {
    if (error_message) {
      *error_message = "invalid dual UYVY frame";
    }
    return false;
  }

  if (converter_ && left_width_ == frame.left_width &&
      left_height_ == frame.left_height &&
      left_stride_bytes_ == frame.left_stride_bytes &&
      right_width_ == frame.right_width &&
      right_height_ == frame.right_height &&
      right_stride_bytes_ == frame.right_stride_bytes) {
    return true;
  }

  std::unique_ptr<DualUyvyToI420StitchCudaConverter> converter(
      new DualUyvyToI420StitchCudaConverter());
  if (!converter->Init(frame.left_width, frame.left_height,
                       frame.left_stride_bytes, frame.right_width,
                       frame.right_height, frame.right_stride_bytes,
                       error_message)) {
    return false;
  }

  converter_ = std::move(converter);
  left_width_ = frame.left_width;
  left_height_ = frame.left_height;
  left_stride_bytes_ = frame.left_stride_bytes;
  right_width_ = frame.right_width;
  right_height_ = frame.right_height;
  right_stride_bytes_ = frame.right_stride_bytes;
  return true;
}

bool DualUyvyFrameConverter::ConvertToI420(
    const rtc_dual_camera::ImageFrame& frame,
    ConvertedI420Frame* output,
    std::string* error_message) {
  if (!output) {
    if (error_message) {
      *error_message = "null converted frame output";
    }
    return false;
  }
  *output = ConvertedI420Frame();

  if (frame.format == rtc_dual_camera::ImagePixelFormat::kI420) {
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

}  // namespace rtc_camera_headless
