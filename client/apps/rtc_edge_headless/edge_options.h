#pragma once

#include "rtc_edge/dual_camera_streaming_module.h"
#include "rtc_headless/rtc_camera_common.h"
#include "rtc_vehicle/vehicle_control_module.h"

#include <chrono>
#include <string>

namespace rtc_edge_headless {

struct SurroundCameraOptions {
  std::string front_device;
  std::string rear_device;
  std::string left_device;
  std::string right_device;
  int width = 0;
  int height = 0;
  int buffer_count = 4;
  int timeout_ms = 2000;
  int warmup_frames = 0;
  int warmup_delay_ms = 0;
  std::chrono::milliseconds frame_wait{20};
};

struct EdgeOptions {
  rtc_camera_headless::CaptureOptions rtc;
  rtc_edge::DualCameraStreamingModuleOptions camera;
  SurroundCameraOptions surround_camera;
  rtc_vehicle::VehicleControlModuleOptions control;
};

EdgeOptions ParseEdgeArgs(int argc, char** argv);
EdgeOptions LoadEdgeOptions(const std::string& config_path);

}  // 命名空间 rtc_edge_headless
