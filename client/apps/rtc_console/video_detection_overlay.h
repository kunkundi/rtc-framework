#pragma once

#include "video_composite_layout.h"

namespace rtc_console {

struct NormalizedRect {
  float left = 0.0f;
  float top = 0.0f;
  float right = 0.0f;
  float bottom = 0.0f;
};

// 检测坐标属于完整源帧；裁剪后映射到实际图像区域，不包含标题或黑边。
bool MapDetectionRectToImage(const NormalizedRect& source_rect,
                             const NormalizedRect& source_crop,
                             const VideoRect& image,
                             VideoRect* mapped);

}  // 命名空间 rtc_console
