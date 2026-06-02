#include "yolo_frame_consumer.h"

#include "rtc_camera_common.h"

#include <chrono>

namespace rtc_camera_headless {
namespace {

void ProcessYoloFrame(const rtc_dual_camera::ImageFrame& frame) {
  (void)frame;
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
