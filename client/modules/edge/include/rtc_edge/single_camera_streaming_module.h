#pragma once

#include "rtc_camera/single/async_image_source.h"

#include <chrono>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

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
            bool continuous,
            std::string* error_message);

  bool started() const;
  uint64_t captured_frames() const;

 private:
  bool StartLocalYuvFile(std::string* error_message);
  bool TickLocalYuvFile(rtc_runtime::RtcSession* rtc_session,
                        bool send_frame,
                        bool continuous,
                        std::string* error_message);

  SingleCameraStreamingModuleOptions options_;
  std::unique_ptr<rtc_camera::single::AsyncCameraImageSource> video_source_;
  std::shared_ptr<rtc_camera::single::CameraFrameSubscription> rtc_frames_;
  std::unique_ptr<rtc_camera::single::CameraFrameConverter> converter_;
  bool use_local_yuv_file_ = false;
  std::ifstream local_yuv_file_;
  std::vector<uint8_t> local_i420_frame_;
  size_t local_frame_size_ = 0;
  size_t local_width_ = 0;
  size_t local_height_ = 0;
  size_t local_stride_y_ = 0;
  size_t local_stride_u_ = 0;
  size_t local_stride_v_ = 0;
  uint64_t local_captured_frames_ = 0;
  std::chrono::steady_clock::time_point local_next_frame_time_{};
  bool started_ = false;
  bool first_frame_logged_ = false;
};

}  // 命名空间 rtc_edge
