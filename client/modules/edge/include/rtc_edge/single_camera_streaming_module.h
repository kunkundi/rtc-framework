#pragma once

#include "rtc_camera/single/async_image_source.h"

#include <chrono>
#include <memory>
#include <string>

namespace rtc_camera {
namespace single {
class CameraFrameConverter;
}  // 命名空间 single
}  // 命名空间 rtc_camera

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
            bool send_frame,
            std::string* error_message);

  bool started() const;
  uint64_t captured_frames() const;

 private:
  SingleCameraStreamingModuleOptions options_;
  std::unique_ptr<rtc_camera::single::AsyncCameraImageSource> video_source_;
  std::shared_ptr<rtc_camera::single::CameraFrameSubscription> rtc_frames_;
  std::unique_ptr<rtc_camera::single::CameraFrameConverter> converter_;
  bool started_ = false;
  bool first_frame_logged_ = false;
};

}  // 命名空间 rtc_edge
