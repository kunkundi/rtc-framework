#pragma once

#include "rtc_dual_camera/dual_camera_async_image_source.h"

#include <atomic>
#include <memory>
#include <thread>

namespace rtc_camera_headless {

class YoloFrameConsumer {
 public:
  explicit YoloFrameConsumer(
      const std::shared_ptr<rtc_dual_camera::AsyncImageFrameSubscription>&
          frames);
  ~YoloFrameConsumer();

  YoloFrameConsumer(const YoloFrameConsumer&) = delete;
  YoloFrameConsumer& operator=(const YoloFrameConsumer&) = delete;

  void Start();
  void Stop();

 private:
  void Run();

  std::atomic<bool> stop_requested_{false};
  std::shared_ptr<rtc_dual_camera::AsyncImageFrameSubscription> frames_;
  std::thread thread_;
};

}  // namespace rtc_camera_headless
