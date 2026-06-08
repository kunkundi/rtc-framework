#include "yolo_frame_consumer.h"

#include <algorithm>
#include <chrono>
#include <memory>
#include <stdint.h>
#include <sstream>
#include <string>
#include <vector>

#include "dual_uyvy_frame_converter.h"
#include "rtc_camera_common.h"
#include "stereo_detection_fuser.h"
#include "vision_detection_sender.h"
#ifdef VTSRTC_ENABLE_YOLO_ONNXRUNTIME
#include "yolo_onnx_detector.h"
#endif

namespace rtc_camera_headless {
namespace {

constexpr bool kSendYoloDetections = true;
constexpr size_t kMaxProcessingDownscale = 8;

struct VisionWorkFrame {
  ConvertedI420Frame frame;
  std::vector<uint8_t> storage;
};

size_t ClampProcessingDownscale(size_t value) {
  return std::max(static_cast<size_t>(1),
                  std::min(value, kMaxProcessingDownscale));
}

bool ValidateI420Frame(const ConvertedI420Frame& frame,
                       std::string* error_message) {
  if (!frame.data || frame.width == 0 || frame.height == 0 ||
      frame.stride_y == 0 || frame.stride_u == 0 || frame.stride_v == 0) {
    if (error_message) {
      *error_message = "invalid I420 frame";
    }
    return false;
  }

  const size_t chroma_height = (frame.height + 1) / 2;
  const size_t required_size =
      frame.stride_y * frame.height + frame.stride_u * chroma_height +
      frame.stride_v * chroma_height;
  if (frame.data_size < required_size) {
    if (error_message) {
      *error_message = "I420 frame buffer is smaller than its strides";
    }
    return false;
  }

  return true;
}

void DownscalePlaneBox(const uint8_t* src,
                       size_t src_stride,
                       size_t src_width,
                       size_t src_height,
                       uint8_t* dst,
                       size_t dst_stride,
                       size_t dst_width,
                       size_t dst_height) {
  for (size_t y = 0; y < dst_height; ++y) {
    const size_t src_y0 = y * src_height / dst_height;
    size_t src_y1 = (y + 1) * src_height / dst_height;
    if (src_y1 <= src_y0) {
      src_y1 = std::min(src_height, src_y0 + 1);
    }
    for (size_t x = 0; x < dst_width; ++x) {
      const size_t src_x0 = x * src_width / dst_width;
      size_t src_x1 = (x + 1) * src_width / dst_width;
      if (src_x1 <= src_x0) {
        src_x1 = std::min(src_width, src_x0 + 1);
      }
      uint32_t sum = 0;
      uint32_t count = 0;
      for (size_t src_y = src_y0; src_y < src_y1; ++src_y) {
        const uint8_t* src_row = src + src_y * src_stride;
        for (size_t src_x = src_x0; src_x < src_x1; ++src_x) {
          sum += src_row[src_x];
          ++count;
        }
      }
      dst[y * dst_stride + x] =
          count > 0 ? static_cast<uint8_t>((sum + count / 2) / count) : 0;
    }
  }
}

bool BuildVisionWorkFrame(const ConvertedI420Frame& source,
                          size_t requested_downscale,
                          VisionWorkFrame* output,
                          std::string* error_message) {
  if (!output) {
    if (error_message) {
      *error_message = "null vision work frame output";
    }
    return false;
  }
  output->frame = ConvertedI420Frame();
  output->storage.clear();

  if (!ValidateI420Frame(source, error_message)) {
    return false;
  }

  const size_t factor = ClampProcessingDownscale(requested_downscale);
  if (factor <= 1) {
    output->frame = source;
    return true;
  }

  const size_t dst_width = (source.width / factor) & ~static_cast<size_t>(1);
  const size_t dst_height = (source.height / factor) & ~static_cast<size_t>(1);
  if (dst_width < 2 || dst_height < 2) {
    if (error_message) {
      *error_message = "vision processing downscale factor is too large";
    }
    return false;
  }

  const size_t src_chroma_width = (source.width + 1) / 2;
  const size_t src_chroma_height = (source.height + 1) / 2;
  const size_t dst_chroma_width = dst_width / 2;
  const size_t dst_chroma_height = dst_height / 2;
  const size_t dst_stride_y = dst_width;
  const size_t dst_stride_u = dst_chroma_width;
  const size_t dst_stride_v = dst_chroma_width;
  const size_t dst_y_bytes = dst_stride_y * dst_height;
  const size_t dst_u_bytes = dst_stride_u * dst_chroma_height;
  const size_t dst_v_bytes = dst_stride_v * dst_chroma_height;

  output->storage.resize(dst_y_bytes + dst_u_bytes + dst_v_bytes);
  uint8_t* dst_y = output->storage.data();
  uint8_t* dst_u = dst_y + dst_y_bytes;
  uint8_t* dst_v = dst_u + dst_u_bytes;

  const size_t src_y_bytes = source.stride_y * source.height;
  const size_t src_u_bytes = source.stride_u * src_chroma_height;
  const uint8_t* src_y = source.data;
  const uint8_t* src_u = src_y + src_y_bytes;
  const uint8_t* src_v = src_u + src_u_bytes;

  DownscalePlaneBox(src_y, source.stride_y, source.width, source.height,
                    dst_y, dst_stride_y, dst_width, dst_height);
  DownscalePlaneBox(src_u, source.stride_u, src_chroma_width,
                    src_chroma_height, dst_u, dst_stride_u, dst_chroma_width,
                    dst_chroma_height);
  DownscalePlaneBox(src_v, source.stride_v, src_chroma_width,
                    src_chroma_height, dst_v, dst_stride_v, dst_chroma_width,
                    dst_chroma_height);

  output->frame.data = output->storage.data();
  output->frame.data_size = output->storage.size();
  output->frame.width = dst_width;
  output->frame.height = dst_height;
  output->frame.stride_y = dst_stride_y;
  output->frame.stride_u = dst_stride_u;
  output->frame.stride_v = dst_stride_v;
  return true;
}

bool RunYoloInference(const rtc_dual_camera::ImageFrame& source_frame,
                      const ConvertedI420Frame& converted_frame,
                      std::vector<YoloDetectionBox>* yolo_boxes) {
#ifdef VTSRTC_ENABLE_YOLO_ONNXRUNTIME
  static bool detector_initialized = false;
  static bool detector_missing_logged = false;
  static bool inference_error_logged = false;
  static std::unique_ptr<YoloOnnxDetector> detector;

  if (!detector_initialized) {
    detector_initialized = true;
    std::string error_message;
    detector = YoloOnnxDetector::CreateDefault(&error_message);
    if (detector) {
      LogInfo(std::string("YOLO ONNX model loaded: ") +
              detector->model_path());
    } else if (!detector_missing_logged) {
      detector_missing_logged = true;
      LogError(
          "YOLO ONNX model is unavailable: " + error_message +
          ". Run `xmake yolo_model` or set VTSRTC_YOLO_MODEL to "
          "models/yolo26n.onnx.");
    }
  }

  if (!detector) {
    return false;
  }

  std::string error_message;
  rtc_dual_camera::ImageFrame i420_frame;
  i420_frame.format = rtc_dual_camera::ImagePixelFormat::kI420;
  i420_frame.sequence = source_frame.sequence;
  i420_frame.timestamp_us = source_frame.timestamp_us;
  i420_frame.width = converted_frame.width;
  i420_frame.height = converted_frame.height;
  i420_frame.stride_y = converted_frame.stride_y;
  i420_frame.stride_u = converted_frame.stride_u;
  i420_frame.stride_v = converted_frame.stride_v;
  i420_frame.data = converted_frame.data;
  i420_frame.data_size = converted_frame.data_size;

  if (!detector->Detect(i420_frame, yolo_boxes, &error_message)) {
    if (!inference_error_logged) {
      inference_error_logged = true;
      LogError(std::string("YOLO inference disabled after error: ") +
               error_message);
    }
    detector.reset();
    return false;
  }

  return true;
#else
  static bool yolo_disabled_logged = false;
  (void)source_frame;
  (void)converted_frame;
  if (yolo_boxes) {
    yolo_boxes->clear();
  }
  if (!yolo_disabled_logged) {
    yolo_disabled_logged = true;
    LogInfo("YOLO inference is not enabled; configure with --enable_yolo=true.");
  }
  return false;
#endif
}

size_t StereoLeftWidth(const rtc_dual_camera::ImageFrame& frame,
                       const ConvertedI420Frame& converted_frame) {
  if (frame.format == rtc_dual_camera::ImagePixelFormat::kDualUyvy &&
      frame.left_width > 0 && frame.right_width > 0) {
    const size_t total_sampled = frame.left_width / 2 + frame.right_width / 2;
    if (total_sampled > 0) {
      return converted_frame.width * (frame.left_width / 2) / total_sampled;
    }
  }
  return converted_frame.width / 2;
}

void ProcessYoloFrame(const rtc_dual_camera::ImageFrame& frame,
                      DualUyvyFrameConverter* frame_converter,
                      const YoloFrameConsumerOptions& options) {
  if (!kSendYoloDetections) {
    (void)frame;
    (void)frame_converter;
    (void)options;
    return;
  }

#ifndef VTSRTC_ENABLE_YOLO_ONNXRUNTIME
  std::vector<YoloDetectionBox> unused_boxes;
  RunYoloInference(frame, ConvertedI420Frame(), &unused_boxes);
  (void)frame_converter;
  (void)options;
  return;
#endif

  ConvertedI420Frame converted_frame;
  std::string error_message;
  if (!frame_converter ||
      !frame_converter->ConvertToI420(frame, &converted_frame,
                                      &error_message)) {
    static bool convert_error_logged = false;
    if (!convert_error_logged) {
      convert_error_logged = true;
      LogError(std::string("YOLO frame conversion disabled after error: ") +
               error_message);
    }
    return;
  }

  VisionWorkFrame work_frame;
  if (!BuildVisionWorkFrame(converted_frame, options.processing_downscale,
                            &work_frame, &error_message)) {
    static bool resize_error_logged = false;
    if (!resize_error_logged) {
      resize_error_logged = true;
      LogError(std::string("YOLO/OpenCV downscale disabled after error: ") +
               error_message);
    }
    return;
  }

  static bool work_frame_logged = false;
  if (!work_frame_logged) {
    work_frame_logged = true;
    std::ostringstream oss;
    oss << "YOLO/OpenCV processing frame " << work_frame.frame.width << "x"
        << work_frame.frame.height << " from " << converted_frame.width << "x"
        << converted_frame.height << ", downscale="
        << ClampProcessingDownscale(options.processing_downscale);
    LogInfo(oss.str());
  }

  std::vector<YoloDetectionBox> yolo_boxes;
  if (!RunYoloInference(frame, work_frame.frame, &yolo_boxes)) {
    return;
  }

  const size_t left_width = StereoLeftWidth(frame, work_frame.frame);
  const std::vector<YoloDetectionBox> fused_boxes =
      FuseStereoDetectionsWithLocalMatching(work_frame.frame, left_width,
                                            yolo_boxes);
  SendYoloDetections(fused_boxes, static_cast<uint32_t>(converted_frame.width),
                     static_cast<uint32_t>(converted_frame.height));
}

}  // namespace

YoloFrameConsumer::YoloFrameConsumer(
    const std::shared_ptr<rtc_dual_camera::AsyncImageFrameSubscription>& frames,
    const YoloFrameConsumerOptions& options)
    : frames_(frames), options_(options) {}

YoloFrameConsumer::~YoloFrameConsumer() { Stop(); }

void YoloFrameConsumer::Start() {
  if (thread_.joinable()) {
    return;
  }
  stop_requested_.store(false);
  thread_ = std::thread(&YoloFrameConsumer::Run, this);
}

void YoloFrameConsumer::Stop() {
  stop_requested_.store(true);
  if (frames_) {
    frames_->Close();
  }
  if (thread_.joinable()) {
    thread_.join();
  }
}

void YoloFrameConsumer::Run() {
  bool first_frame_logged = false;
  DualUyvyFrameConverter frame_converter;
  while (!stop_requested_.load() && !StopRequested()) {
    rtc_dual_camera::ImageFrame frame;
    if (!frames_ || !frames_->WaitNext(&frame, std::chrono::milliseconds(50))) {
      continue;
    }

    if (frame.empty()) {
      continue;
    }

    if (!first_frame_logged) {
      first_frame_logged = true;
      LogInfo("first raw frame received by YOLO consumer");
    }

    ProcessYoloFrame(frame, &frame_converter, options_);
  }
}

}  // namespace rtc_camera_headless
