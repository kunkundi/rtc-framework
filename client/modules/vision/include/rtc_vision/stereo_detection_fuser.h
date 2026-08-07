#pragma once

#include "rtc_vision/vision_detection_sender.h"

#include <stddef.h>
#include <stdint.h>

#include <vector>

namespace rtc_camera_headless {

struct StereoLumaFrame {
  const uint8_t* data = nullptr;
  size_t width = 0;
  size_t height = 0;
  size_t stride = 0;
};

std::vector<YoloDetectionBox> FuseStereoDetectionsWithLocalMatching(
    const StereoLumaFrame& frame,
    size_t left_width,
    const std::vector<YoloDetectionBox>& detections);

}  // namespace rtc_camera_headless
