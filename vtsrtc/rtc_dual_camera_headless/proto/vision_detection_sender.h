#pragma once

#include <stdint.h>

#include <vector>

namespace rtc_camera_headless {

struct YoloDetectionBox {
  uint32_t class_id = 0;
  uint32_t confidence = 0;
  uint32_t x = 0;
  uint32_t y = 0;
  uint32_t w = 0;
  uint32_t h = 0;
  bool has_detection_id = false;
  uint32_t detection_id = 0;
  bool has_track_id = false;
  uint32_t track_id = 0;
  uint32_t flags = 0;
};

void SendYoloDetections(const std::vector<YoloDetectionBox>& yolo_boxes);

}  // namespace rtc_camera_headless
