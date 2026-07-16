#pragma once

#include "rtc_dual_camera/dual_camera_async_image_source.h"

#include <chrono>
#include <memory>
#include <string>

namespace rtc_camera_headless {
class DualUyvyFrameConverter;
class YoloFrameConsumer;
}  // 命名空间 rtc_camera_headless

namespace rtc_runtime {
class RtcSession;
}  // 命名空间 rtc_runtime

namespace rtc_edge {

struct DualCameraStreamingModuleOptions {
  rtc_dual_camera::AsyncDualCameraImageSourceOptions capture;
  std::chrono::milliseconds frame_wait{20};
  bool yolo_enabled = false;
  size_t yolo_processing_downscale = 1;
};

class DualCameraStreamingModule {
 public:
  explicit DualCameraStreamingModule(
      const DualCameraStreamingModuleOptions& options);
  ~DualCameraStreamingModule();

  DualCameraStreamingModule(const DualCameraStreamingModule&) = delete;
  DualCameraStreamingModule& operator=(
      const DualCameraStreamingModule&) = delete;

  bool Start(std::string* error_message);
  void Stop();
  bool Tick(rtc_runtime::RtcSession* rtc_session,
            std::string* error_message);

  bool started() const { return started_; }
  uint64_t captured_frames() const;

 private:
  DualCameraStreamingModuleOptions options_;
  std::unique_ptr<rtc_dual_camera::AsyncDualCameraImageSource> video_source_;
  std::shared_ptr<rtc_dual_camera::AsyncImageFrameSubscription> rtc_frames_;
  std::shared_ptr<rtc_dual_camera::AsyncImageFrameSubscription> yolo_frames_;
  std::unique_ptr<rtc_camera_headless::DualUyvyFrameConverter> converter_;
  std::unique_ptr<rtc_camera_headless::YoloFrameConsumer> yolo_consumer_;
  bool started_ = false;
  bool first_frame_logged_ = false;
};

}  // 命名空间 rtc_edge
