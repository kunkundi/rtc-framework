#include "dual_uyvy_frame_converter.h"

#include "dual_uyvy_to_i420_stitch_cuda.h"

#include <linux/videodev2.h>
#include <cstring>
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
                       frame.left_stride_bytes,
                       frame.left_pixel_format == V4L2_PIX_FMT_YUYV,
                       frame.right_width, frame.right_height,
                       frame.right_stride_bytes,
                       frame.right_pixel_format == V4L2_PIX_FMT_YUYV,
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

  // NV12 cameras: use CPU deinterleave + stitch path
  if (frame.left_pixel_format == V4L2_PIX_FMT_NV12 ||
      frame.right_pixel_format == V4L2_PIX_FMT_NV12) {
    return ConvertNv12DualToI420(frame, output, error_message);
  }

  // UYVY / YUYV cameras: use CUDA conversion path
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

namespace {

bool ConvertSingleNv12ToI420(const uint8_t* nv12_data,
                             size_t nv12_size,
                             size_t width,
                             size_t height,
                             size_t stride_bytes,
                             std::vector<uint8_t>* y_plane,
                             std::vector<uint8_t>* u_plane,
                             std::vector<uint8_t>* v_plane,
                             std::string* error_message) {
  if (!nv12_data || !y_plane || !u_plane || !v_plane) {
    if (error_message) {
      *error_message = "null NV12 conversion parameter";
    }
    return false;
  }

  // NV12 layout: Y plane at stride*height, followed by interleaved UV
  const size_t y_plane_size = stride_bytes * height;
  const size_t uv_plane_size = stride_bytes * (height / 2);
  const size_t expected_size = y_plane_size + uv_plane_size;

  if (nv12_size < expected_size) {
    if (error_message) {
      *error_message = "NV12 buffer too small for " +
                       std::to_string(width) + "x" +
                       std::to_string(height);
    }
    return false;
  }

  // Copy Y plane row-by-row (handle stride > width padding)
  y_plane->resize(width * height);
  for (size_t row = 0; row < height; ++row) {
    std::memcpy(y_plane->data() + row * width,
                nv12_data + row * stride_bytes, width);
  }

  // Deinterleave UV plane (UVUVUV... → UUU... + VVV...)
  const uint8_t* uv_src = nv12_data + y_plane_size;
  const size_t uv_rows = height / 2;
  const size_t uv_cols = width / 2;
  u_plane->resize(uv_cols * uv_rows);
  v_plane->resize(uv_cols * uv_rows);

  for (size_t row = 0; row < uv_rows; ++row) {
    for (size_t col = 0; col < uv_cols; ++col) {
      const size_t src_off = row * stride_bytes + col * 2;
      const size_t dst_off = row * uv_cols + col;
      (*u_plane)[dst_off] = uv_src[src_off];
      (*v_plane)[dst_off] = uv_src[src_off + 1];
    }
  }

  return true;
}

}  // namespace

bool DualUyvyFrameConverter::ConvertNv12DualToI420(
    const rtc_dual_camera::ImageFrame& frame,
    ConvertedI420Frame* output,
    std::string* error_message) {
  if (frame.format != rtc_dual_camera::ImagePixelFormat::kDualUyvy) {
    if (error_message) {
      *error_message = "NV12 conversion requires a dual-camera frame";
    }
    return false;
  }

  // Convert left camera NV12 → planar Y/U/V
  if (!ConvertSingleNv12ToI420(frame.left_data, frame.left_data_size,
                               frame.left_width, frame.left_height,
                               frame.left_stride_bytes, &left_y_, &left_u_,
                               &left_v_, error_message)) {
    return false;
  }

  // Convert right camera NV12 → planar Y/U/V
  if (!ConvertSingleNv12ToI420(frame.right_data, frame.right_data_size,
                               frame.right_width, frame.right_height,
                               frame.right_stride_bytes, &right_y_, &right_u_,
                               &right_v_, error_message)) {
    return false;
  }

  // Stitch side-by-side I420
  const size_t out_width = frame.left_width + frame.right_width;
  const size_t out_height = frame.left_height;
  const size_t out_y_stride = out_width;
  const size_t out_u_stride = out_width / 2;
  const size_t out_v_stride = out_width / 2;
  const size_t chroma_height = (out_height + 1) / 2;
  const size_t y_size = out_y_stride * out_height;
  const size_t u_size = out_u_stride * chroma_height;
  const size_t v_size = out_v_stride * chroma_height;

  stitched_i420_.resize(y_size + u_size + v_size);
  uint8_t* dst_y = stitched_i420_.data();
  uint8_t* dst_u = dst_y + y_size;
  uint8_t* dst_v = dst_u + u_size;

  const size_t left_chroma_h = (frame.left_height + 1) / 2;
  const size_t right_chroma_h = (frame.right_height + 1) / 2;

  // Stitch Y planes
  for (size_t row = 0; row < out_height; ++row) {
    std::memcpy(dst_y + row * out_y_stride,
                left_y_.data() + row * frame.left_width, frame.left_width);
    std::memcpy(dst_y + row * out_y_stride + frame.left_width,
                right_y_.data() + row * frame.right_width,
                frame.right_width);
  }

  // Stitch U planes
  for (size_t row = 0; row < chroma_height; ++row) {
    const size_t dst_row = row * out_u_stride;
    if (row < left_chroma_h) {
      std::memcpy(dst_u + dst_row,
                  left_u_.data() + row * (frame.left_width / 2),
                  frame.left_width / 2);
    }
    if (row < right_chroma_h) {
      std::memcpy(dst_u + dst_row + (frame.left_width / 2),
                  right_u_.data() + row * (frame.right_width / 2),
                  frame.right_width / 2);
    }
  }

  // Stitch V planes
  for (size_t row = 0; row < chroma_height; ++row) {
    const size_t dst_row = row * out_v_stride;
    if (row < left_chroma_h) {
      std::memcpy(dst_v + dst_row,
                  left_v_.data() + row * (frame.left_width / 2),
                  frame.left_width / 2);
    }
    if (row < right_chroma_h) {
      std::memcpy(dst_v + dst_row + (frame.left_width / 2),
                  right_v_.data() + row * (frame.right_width / 2),
                  frame.right_width / 2);
    }
  }

  output->data = stitched_i420_.data();
  output->data_size = stitched_i420_.size();
  output->width = out_width;
  output->height = out_height;
  output->stride_y = out_y_stride;
  output->stride_u = out_u_stride;
  output->stride_v = out_v_stride;
  return true;
}

}  // namespace rtc_camera_headless
