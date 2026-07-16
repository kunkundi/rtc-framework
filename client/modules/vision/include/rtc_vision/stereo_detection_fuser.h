#pragma once

#include "rtc_camera/dual/frame_converter.h"
#include "rtc_vision/vision_detection_sender.h"

#include <stddef.h>

#include <vector>

namespace rtc_camera_headless {

std::vector<YoloDetectionBox> FuseStereoDetectionsWithLocalMatching(
    const rtc_camera::dual::ConvertedI420Frame& frame,
    size_t left_width,
    const std::vector<YoloDetectionBox>& detections);

}  // namespace rtc_camera_headless
