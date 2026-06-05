#include "yolo_frame_consumer.h"

#include <chrono>
#include <vector>

#include "rtc_camera_common.h"
#include "vision_detection_sender.h"
#ifdef VTSRTC_ENABLE_YOLO_ONNXRUNTIME
#include "yolo_onnx_detector.h"
#endif

namespace rtc_camera_headless {
namespace {

constexpr bool kSendYoloDetections = true;

bool RunYoloInference(const rtc_dual_camera::ImageFrame& frame,
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
  if (!detector->Detect(frame, yolo_boxes, &error_message)) {
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

void ProcessYoloFrame(const rtc_dual_camera::ImageFrame& frame) {
  if (!kSendYoloDetections) {
    (void)frame;
    return;
  }

  std::vector<YoloDetectionBox> yolo_boxes;
  if (RunYoloInference(frame, &yolo_boxes)) {
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
  while (!stop_requested_.load() && !StopRequested()) {
    rtc_dual_camera::ImageFrame frame;
    if (!frames_ || !frames_->WaitNext(&frame, std::chrono::milliseconds(50))) {
      continue;
    }

    if (frame.format != rtc_dual_camera::ImagePixelFormat::kI420 ||
        frame.empty()) {
      continue;
    }

    if (!first_frame_logged) {
      first_frame_logged = true;
      LogInfo("first stitched frame received by YOLO consumer");
    }

    ProcessYoloFrame(frame);
  }
}

}  // namespace rtc_camera_headless
