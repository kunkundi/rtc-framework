#include "yolo_frame_consumer.h"

#include "rtc_camera_common.h"
#include "vision_detection_sender.h"

#include <chrono>
#include <vector>

namespace rtc_camera_headless {
namespace {

constexpr bool kSendYoloDetections = true;

std::vector<YoloDetectionBox> RunYoloInference(
    const rtc_dual_camera::ImageFrame& frame) {
  const uint32_t phase =
      static_cast<uint32_t>((frame.sequence * 1800) % 24000);

  YoloDetectionBox primary_box;
  primary_box.class_id = 0;
  primary_box.confidence = 920;
  primary_box.x = 6000 + phase;
  primary_box.y = 11000;
  primary_box.w = 18000;
  primary_box.h = 22000;
  primary_box.has_detection_id = true;
  primary_box.detection_id = static_cast<uint32_t>(frame.sequence * 2);

  YoloDetectionBox secondary_box;
  secondary_box.class_id = 1;
  secondary_box.confidence = 780;
  secondary_box.x = 38000 - phase / 2;
  secondary_box.y = 26000;
  secondary_box.w = 14000;
  secondary_box.h = 18000;
  secondary_box.has_detection_id = true;
  secondary_box.detection_id = static_cast<uint32_t>(frame.sequence * 2 + 1);

  return {primary_box, secondary_box};
}

void ProcessYoloFrame(const rtc_dual_camera::ImageFrame& frame) {
  if (!kSendYoloDetections) {
    (void)frame;
    return;
  }

  const std::vector<YoloDetectionBox> yolo_boxes =
      RunYoloInference(frame);
  SendYoloDetections(yolo_boxes);
}

}  // namespace

YoloFrameConsumer::YoloFrameConsumer(
    const std::shared_ptr<rtc_dual_camera::AsyncImageFrameSubscription>& frames)
    : frames_(frames) {}

YoloFrameConsumer::~YoloFrameConsumer() {
  Stop();
}

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
    if (!frames_ ||
        !frames_->WaitNext(&frame, std::chrono::milliseconds(50))) {
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
