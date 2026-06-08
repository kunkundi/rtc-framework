#include "yolo_frame_consumer.h"

#include <chrono>
#include <string>
#include <vector>

#include "dual_uyvy_frame_converter.h"
#include "rtc_camera_common.h"
#include "vision_detection_sender.h"
#ifdef VTSRTC_ENABLE_YOLO_ONNXRUNTIME
#include "yolo_onnx_detector.h"
#endif

namespace rtc_camera_headless {
namespace {

constexpr bool kSendYoloDetections = true;

bool RunYoloInference(const rtc_dual_camera::ImageFrame& frame,
                      DualUyvyFrameConverter* frame_converter,
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

  if (!frame_converter) {
    if (!inference_error_logged) {
      inference_error_logged = true;
      LogError("YOLO inference disabled after error: null frame converter");
    }
    detector.reset();
    return false;
  }

  ConvertedI420Frame converted_frame;
  std::string error_message;
  if (!frame_converter->ConvertToI420(frame, &converted_frame,
                                      &error_message)) {
    if (!inference_error_logged) {
      inference_error_logged = true;
      LogError(std::string("YOLO inference disabled after error: ") +
               error_message);
    }
    detector.reset();
    return false;
  }

  rtc_dual_camera::ImageFrame i420_frame;
  i420_frame.format = rtc_dual_camera::ImagePixelFormat::kI420;
  i420_frame.sequence = frame.sequence;
  i420_frame.timestamp_us = frame.timestamp_us;
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
  (void)frame;
  (void)frame_converter;
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

void ProcessYoloFrame(const rtc_dual_camera::ImageFrame& frame,
                      DualUyvyFrameConverter* frame_converter) {
  if (!kSendYoloDetections) {
    (void)frame;
    (void)frame_converter;
    return;
  }

  std::vector<YoloDetectionBox> yolo_boxes;
  if (RunYoloInference(frame, frame_converter, &yolo_boxes)) {
    SendYoloDetections(yolo_boxes, static_cast<uint32_t>(frame.width),
                       static_cast<uint32_t>(frame.height));
  }
}

}  // namespace

YoloFrameConsumer::YoloFrameConsumer(
    const std::shared_ptr<rtc_dual_camera::AsyncImageFrameSubscription>& frames)
    : frames_(frames) {}

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

    ProcessYoloFrame(frame, &frame_converter);
  }
}

}  // namespace rtc_camera_headless
