#pragma once

#include <string>

namespace rtc_camera {

struct CameraCaptureOptions {
  std::string device;
  int width = 0;
  int height = 0;
  int buffer_count = 4;
  int timeout_ms = 2000;
  int warmup_frames = 0;
  int warmup_delay_ms = 0;
};

}  // 命名空间 rtc_camera
