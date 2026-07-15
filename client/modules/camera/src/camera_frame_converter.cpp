#include "rtc_camera/camera_frame_converter.h"

#include "rtc_camera/uyvy_to_i420_cuda.h"

#include <linux/videodev2.h>

#include <cstring>
#include <utility>

namespace rtc_camera {

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

  if (frame.pixel_format == V4L2_PIX_FMT_NV12) {
    return ConvertNv12(frame, output, error_message);
  }
  if (frame.pixel_format != V4L2_PIX_FMT_UYVY &&
      frame.pixel_format != V4L2_PIX_FMT_YUYV) {
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
  if (!cuda_converter_->Convert(frame.data, frame.data_size, &converted_data,
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

bool CameraFrameConverter::ConvertNv12(
    const CameraFrame& frame,
    ConvertedCameraFrame* output,
    std::string* error_message) {
  const size_t y_source_size = frame.stride_bytes * frame.height;
  const size_t chroma_height = (frame.height + 1) / 2;
  const size_t uv_source_size = frame.stride_bytes * chroma_height;
  if (frame.data_size < y_source_size + uv_source_size) {
    if (error_message != nullptr) {
      *error_message = "NV12 camera frame is smaller than expected";
    }
    return false;
  }

  const size_t stride_y = frame.width;
  const size_t stride_u = frame.width / 2;
  const size_t stride_v = frame.width / 2;
  const size_t y_size = stride_y * frame.height;
  const size_t u_size = stride_u * chroma_height;
  const size_t v_size = stride_v * chroma_height;
  nv12_i420_.resize(y_size + u_size + v_size);

  uint8_t* destination_y = nv12_i420_.data();
  uint8_t* destination_u = destination_y + y_size;
  uint8_t* destination_v = destination_u + u_size;

  for (size_t row = 0; row < frame.height; ++row) {
    std::memcpy(destination_y + row * stride_y,
                frame.data + row * frame.stride_bytes, frame.width);
  }

  const uint8_t* source_uv = frame.data + y_source_size;
  for (size_t row = 0; row < chroma_height; ++row) {
    for (size_t column = 0; column < frame.width / 2; ++column) {
      const size_t source_offset = row * frame.stride_bytes + column * 2;
      const size_t destination_offset = row * stride_u + column;
      destination_u[destination_offset] = source_uv[source_offset];
      destination_v[destination_offset] = source_uv[source_offset + 1];
    }
  }

  output->data = nv12_i420_.data();
  output->data_size = nv12_i420_.size();
  output->width = frame.width;
  output->height = frame.height;
  output->stride_y = stride_y;
  output->stride_u = stride_u;
  output->stride_v = stride_v;
  return true;
}

bool CameraFrameConverter::EnsureCudaConverter(
    const CameraFrame& frame,
    std::string* error_message) {
  if (cuda_converter_ && cuda_pixel_format_ == frame.pixel_format &&
      cuda_width_ == frame.width && cuda_height_ == frame.height &&
      cuda_stride_bytes_ == frame.stride_bytes) {
    return true;
  }

  std::unique_ptr<UyvyToI420CudaConverter> converter(
      new UyvyToI420CudaConverter());
  const bool is_yuyv = frame.pixel_format == V4L2_PIX_FMT_YUYV;
  if (!converter->Init(frame.width, frame.height, frame.stride_bytes, is_yuyv,
                       error_message)) {
    return false;
  }

  cuda_converter_ = std::move(converter);
  cuda_pixel_format_ = frame.pixel_format;
  cuda_width_ = frame.width;
  cuda_height_ = frame.height;
  cuda_stride_bytes_ = frame.stride_bytes;
  return true;
}

}  // 命名空间 rtc_camera
