#pragma once

#include "rtc_camera/dual/async_image_source.h"

#include <chrono>
#include <memory>
#include <string>

namespace rtc_camera_headless {
class YoloFrameConsumer;
}  // 命名空间 rtc_camera_headless

namespace rtc_camera {
namespace dual {
class DualUyvyFrameConverter;
}  // 命名空间 dual
}  // 命名空间 rtc_camera

namespace rtc_runtime {
class RtcSession;
}  // 命名空间 rtc_runtime

namespace rtc_edge {

struct DualCameraStreamingModuleOptions {
  rtc_camera::dual::AsyncDualCameraImageSourceOptions capture;
  std::chrono::milliseconds frame_wait{20};
  bool yolo_enabled = true;
  size_t yolo_max_fps = 15;
  size_t yolo_processing_downscale = 2;
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
<<<<<<< HEAD
            std::string* error_message);
=======
            std::string* error_message,
            bool send_frame = true,
            bool run_yolo = true);
>>>>>>> 5c8f59e (新加双目和环路切换)

  bool started() const { return started_; }
  uint64_t captured_frames() const;

 private:
<<<<<<< HEAD
=======
  bool StartYoloConsumer(std::string* error_message);
  void StopYoloConsumer();
  bool DrainLatestFrame(rtc_camera::dual::ImageFrame* frame,
                        std::chrono::milliseconds first_wait) const;

>>>>>>> 5c8f59e (新加双目和环路切换)
  DualCameraStreamingModuleOptions options_;
  std::unique_ptr<rtc_camera::dual::AsyncDualCameraImageSource> video_source_;
  std::shared_ptr<rtc_camera::dual::AsyncImageFrameSubscription> rtc_frames_;
  std::shared_ptr<rtc_camera::dual::AsyncImageFrameSubscription> yolo_frames_;
  std::unique_ptr<rtc_camera::dual::DualUyvyFrameConverter> converter_;
  std::unique_ptr<rtc_camera_headless::YoloFrameConsumer> yolo_consumer_;
  bool started_ = false;
  bool first_frame_logged_ = false;
<<<<<<< HEAD
=======
  bool yolo_active_ = false;
>>>>>>> 5c8f59e (新加双目和环路切换)
};

}  // 命名空间 rtc_edge
