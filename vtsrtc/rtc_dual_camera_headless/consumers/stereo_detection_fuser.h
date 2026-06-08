#pragma once

#include "dual_uyvy_frame_converter.h"
#include "vision_detection_sender.h"

#include <stddef.h>

#include <vector>

namespace rtc_camera_headless {

std::vector<YoloDetectionBox> FuseStereoDetectionsWithLocalMatching(
    const ConvertedI420Frame& frame,
    size_t left_width,
    const std::vector<YoloDetectionBox>& detections);

}  // namespace rtc_camera_headless
