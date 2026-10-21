#include "video_detection_overlay.h"

#include <algorithm>
#include <cmath>

namespace rtc_console {
namespace {

bool IsValidRect(const NormalizedRect& rect) {
  return std::isfinite(rect.left) && std::isfinite(rect.top) &&
         std::isfinite(rect.right) && std::isfinite(rect.bottom) &&
         rect.right > rect.left && rect.bottom > rect.top;
}

}  // 匿名命名空间

bool MapDetectionRectToImage(const NormalizedRect& source_rect,
                             const NormalizedRect& source_crop,
                             const VideoRect& image,
                             VideoRect* mapped) {
  if (!mapped || !IsValidRect(source_rect) || !IsValidRect(source_crop) ||
      source_crop.left < 0.0f || source_crop.top < 0.0f ||
      source_crop.right > 1.0f || source_crop.bottom > 1.0f ||
      !std::isfinite(image.x) || !std::isfinite(image.y) ||
      !std::isfinite(image.width) || !std::isfinite(image.height) ||
      image.width <= 0.0f || image.height <= 0.0f) {
    return false;
  }

  const float left = std::max(source_rect.left, source_crop.left);
  const float top = std::max(source_rect.top, source_crop.top);
  const float right = std::min(source_rect.right, source_crop.right);
  const float bottom = std::min(source_rect.bottom, source_crop.bottom);
  if (right <= left || bottom <= top) return false;

  const float scale_x = image.width / (source_crop.right - source_crop.left);
  const float scale_y = image.height / (source_crop.bottom - source_crop.top);
  mapped->x = image.x + (left - source_crop.left) * scale_x;
  mapped->y = image.y + (top - source_crop.top) * scale_y;
  mapped->width = (right - left) * scale_x;
  mapped->height = (bottom - top) * scale_y;
  return true;
}

}  // 命名空间 rtc_console
