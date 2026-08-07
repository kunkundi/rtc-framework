#include "rtc_camera/single/frame_converter.h"

#include "internal/uyvy_to_i420_cuda.h"

#include <linux/videodev2.h>

namespace rtc_camera {
namespace single {

CameraFrameConverter::CameraFrameConverter() = default;
CameraFrameConverter::~CameraFrameConverter() = default;

bool CameraFrameConverter::ConvertToI420(
    const CameraFrame& frame,
    ConvertedCameraFrame* output,
    std::string* error_message) {
  if (output == nullptr) {
    if (error_message != nullptr) {
      *error_message = "Converted frame output must not be null";
    }
    return false;
  }
  *output = ConvertedCameraFrame();
  DiscardPending();

  if (frame.empty() || frame.width == 0 || frame.height == 0 ||
      frame.stride_bytes == 0) {
    if (error_message != nullptr) {
      *error_message = "Camera frame is invalid";
    }
    return false;
  }
  if ((frame.width % 2) != 0 || (frame.height % 2) != 0) {
    if (error_message != nullptr) {
      *error_message = "Camera frame width and height must be even";
    }
    return false;
  }

  if (frame.pixel_format != V4L2_PIX_FMT_UYVY &&
      frame.pixel_format != V4L2_PIX_FMT_YUYV &&
      frame.pixel_format != V4L2_PIX_FMT_NV12) {
    if (error_message != nullptr) {
      *error_message = "Unsupported camera pixel format";
    }
    return false;
  }

  if (!EnsureCudaConverter(frame, error_message)) {
    return false;
  }

  const uint8_t* converted_data = nullptr;
  size_t converted_size = 0;
  if (!cuda_converter_->Convert(frame.data, frame.data_size,
                                frame.mmap_backed_, &converted_data,
                                &converted_size, error_message)) {
    return false;
  }

  output->data = converted_data;
  output->data_size = converted_size;
  output->width = frame.width;
  output->height = frame.height;
  output->stride_y = cuda_converter_->y_stride();
  output->stride_u = cuda_converter_->u_stride();
  output->stride_v = cuda_converter_->v_stride();
  return true;
}

bool CameraFrameConverter::EnqueueToI420(
    const CameraFrame& frame,
    std::string* error_message) {
  if (frame.empty() || frame.width == 0 || frame.height == 0 ||
      frame.stride_bytes == 0 || (frame.width % 2) != 0 ||
      (frame.height % 2) != 0) {
    if (error_message) {
      *error_message = "Camera frame is invalid";
    }
    return false;
  }
  if (frame.pixel_format != V4L2_PIX_FMT_UYVY &&
      frame.pixel_format != V4L2_PIX_FMT_YUYV &&
      frame.pixel_format != V4L2_PIX_FMT_NV12) {
    if (error_message) {
      *error_message = "Unsupported asynchronous camera pixel format";
    }
    return false;
  }
  if (!EnsureCudaConverter(frame, error_message) ||
      !cuda_converter_->Enqueue(frame.data, frame.data_size,
                                frame.mmap_backed_, error_message)) {
    return false;
  }
  pending_cuda_frames_.push_back(frame);
  return true;
}

bool CameraFrameConverter::DequeueI420(
    ConvertedCameraFrame* output,
    std::string* error_message) {
  if (!output) {
    if (error_message) {
      *error_message = "Converted frame output must not be null";
    }
    return false;
  }
  *output = ConvertedCameraFrame();
  if (!cuda_converter_ || pending_cuda_frames_.empty()) {
    if (error_message) {
      *error_message = "No asynchronous camera frame is pending";
    }
    return false;
  }

  const uint8_t* converted_data = nullptr;
  size_t converted_size = 0;
  if (!cuda_converter_->Dequeue(&converted_data, &converted_size,
                                error_message)) {
    return false;
  }
  const CameraFrame frame = pending_cuda_frames_.front();
  pending_cuda_frames_.pop_front();
  output->data = converted_data;
  output->data_size = converted_size;
  output->width = frame.width;
  output->height = frame.height;
  output->stride_y = cuda_converter_->y_stride();
  output->stride_u = cuda_converter_->u_stride();
  output->stride_v = cuda_converter_->v_stride();
  return true;
}

void CameraFrameConverter::DiscardPending() {
  if (cuda_converter_) {
    cuda_converter_->DiscardPending();
  }
  pending_cuda_frames_.clear();
}

size_t CameraFrameConverter::pending_frames() const {
  return pending_cuda_frames_.size();
}

bool CameraFrameConverter::EnsureCudaConverter(
    const CameraFrame& frame,
    std::string* error_message) {
  if (cuda_converter_ && cuda_pixel_format_ == frame.pixel_format &&
      cuda_width_ == frame.width && cuda_height_ == frame.height &&
      cuda_stride_bytes_ == frame.stride_bytes) {
    return true;
  }

  DiscardPending();

  std::unique_ptr<UyvyToI420CudaConverter> converter(
      new UyvyToI420CudaConverter());
  const bool is_yuyv = frame.pixel_format == V4L2_PIX_FMT_YUYV;
  const bool is_nv12 = frame.pixel_format == V4L2_PIX_FMT_NV12;
  if (!converter->Init(frame.width, frame.height, frame.stride_bytes, is_yuyv,
                       is_nv12, error_message)) {
    return false;
  }

  cuda_converter_ = std::move(converter);
  mapped_device_owner_ = frame.mmap_backed_ ? frame.device_owner_
                                            : std::shared_ptr<const void>();
  cuda_pixel_format_ = frame.pixel_format;
  cuda_width_ = frame.width;
  cuda_height_ = frame.height;
  cuda_stride_bytes_ = frame.stride_bytes;
  return true;
}

}  // 命名空间 single
}  // 命名空间 rtc_camera
