#pragma once

namespace rtc_edge {

// 每个视频源 ID 对应一路独立的 RTC 视频通道。
constexpr const char* kStereoCameraVideoSourceId = "merged_image";
constexpr const char* kSurroundFrontVideoSourceId = "surround_front";
constexpr const char* kSurroundRearVideoSourceId = "surround_rear";
constexpr const char* kSurroundLeftVideoSourceId = "surround_left";
constexpr const char* kSurroundRightVideoSourceId = "surround_right";

}  // 命名空间 rtc_edge
