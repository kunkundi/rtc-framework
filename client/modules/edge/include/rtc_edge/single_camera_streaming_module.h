#pragma once

#include "rtc_camera/async_camera_image_source.h"

#include <chrono>
#include <memory>
#include <string>

namespace rtc_camera {
class CameraFrameConverter;
}

namespace rtc_runtime {
class RtcSession;
}

namespace rtc_edge {

struct SingleCameraStreamingModuleOptions {
  rtc_camera::CameraCaptureOptions capture;
  std::string video_source_id;
  std::string camera_name;
  std::chrono::milliseconds frame_wait{10};
};

class SingleCameraStreamingModule {
 public:
  explicit SingleCameraStreamingModule(
      const SingleCameraStreamingModuleOptions& options);
  ~SingleCameraStreamingModule();

  SingleCameraStreamingModule(const SingleCameraStreamingModule&) = delete;
  SingleCameraStreamingModule& operator=(
      const SingleCameraStreamingModule&) = delete;

  bool Start(std::string* error_message);
  void RequestStop();
  void Stop();
  bool Tick(rtc_runtime::RtcSession* rtc_session,
            std::string* error_message);

  bool started() const;
  uint64_t captured_frames() const;

 private:
  SingleCameraStreamingModuleOptions options_;
  std::unique_ptr<rtc_camera::AsyncCameraImageSource> video_source_;
  std::shared_ptr<rtc_camera::CameraFrameSubscription> rtc_frames_;
  std::unique_ptr<rtc_camera::CameraFrameConverter> converter_;
  bool started_ = false;
  bool first_frame_logged_ = false;
};

}  // 命名空间 rtc_edge
