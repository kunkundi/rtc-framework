#pragma once

#include "rtc_camera/dual/async_image_source.h"

#include <stddef.h>

#include <atomic>
#include <memory>
#include <thread>

namespace rtc_camera_headless {

struct YoloFrameConsumerOptions {
  size_t max_fps = 15;
  size_t processing_downscale = 2;
};

class YoloFrameConsumer {
 public:
  explicit YoloFrameConsumer(
      const std::shared_ptr<rtc_camera::dual::AsyncImageFrameSubscription>&
          frames,
      const YoloFrameConsumerOptions& options = YoloFrameConsumerOptions());
  ~YoloFrameConsumer();

  YoloFrameConsumer(const YoloFrameConsumer&) = delete;
  YoloFrameConsumer& operator=(const YoloFrameConsumer&) = delete;

  void Start();
  void Stop();

 private:
  void Run();

  std::atomic<bool> stop_requested_{false};
  std::shared_ptr<rtc_camera::dual::AsyncImageFrameSubscription> frames_;
  YoloFrameConsumerOptions options_;
  std::thread thread_;
};

}  // namespace rtc_camera_headless
