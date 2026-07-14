#pragma once

#include "rtc_dual_camera/dual_camera_async_image_source.h"

#include <chrono>
#include <memory>
#include <string>

namespace rtc_camera_headless {
class DualUyvyFrameConverter;
class RtcHeadlessSession;
}  // 命名空间 rtc_camera_headless

namespace rtc_vehicle {

struct VehicleCameraModuleOptions {
  rtc_dual_camera::AsyncDualCameraImageSourceOptions capture;
  std::chrono::milliseconds frame_wait{20};
};

class VehicleCameraModule {
 public:
  explicit VehicleCameraModule(const VehicleCameraModuleOptions& options);
  ~VehicleCameraModule();

  VehicleCameraModule(const VehicleCameraModule&) = delete;
  VehicleCameraModule& operator=(const VehicleCameraModule&) = delete;

  bool Start(std::string* error_message);
  void Stop();
  bool Tick(rtc_camera_headless::RtcHeadlessSession* rtc_session,
            std::string* error_message);

  bool started() const { return started_; }
  uint64_t captured_frames() const;

 private:
  VehicleCameraModuleOptions options_;
  std::unique_ptr<rtc_dual_camera::AsyncDualCameraImageSource> video_source_;
  std::shared_ptr<rtc_dual_camera::AsyncImageFrameSubscription> rtc_frames_;
  std::unique_ptr<rtc_camera_headless::DualUyvyFrameConverter> converter_;
  bool started_ = false;
  bool first_frame_logged_ = false;
};

}  // 命名空间 rtc_vehicle
