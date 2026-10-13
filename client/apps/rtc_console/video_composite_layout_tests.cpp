#include "video_composite_layout.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void Check(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << std::endl;
    std::exit(1);
  }
}

bool NearlyEqual(float lhs, float rhs) {
  return std::fabs(lhs - rhs) < 0.01f;
}

bool IsInside(const rtc_console::VideoRect& inner,
              const rtc_console::VideoRect& outer) {
  return inner.x >= outer.x && inner.y >= outer.y &&
         inner.x + inner.width <= outer.x + outer.width + 0.01f &&
         inner.y + inner.height <= outer.y + outer.height + 0.01f;
}

void TestBinocularLayoutUsesSurroundGridOverlay() {
  const rtc_console::VideoCompositeLayout layout =
      rtc_console::ComputeVideoCompositeLayout(true, 1000.0f, 600.0f,
                                               32.0f / 9.0f);
  Check(layout.placements.size() == 5, "binocular layout tile count");
  const rtc_console::VideoTilePlacement& stereo = layout.placements[0];
  Check(stereo.tile == rtc_console::VideoTileId::Stereo,
        "stereo is binocular main view");
  Check(!stereo.picture_in_picture, "stereo is not an overlay");
  Check(NearlyEqual(stereo.rect.width, 1000.0f) &&
            NearlyEqual(stereo.rect.height, 600.0f),
        "stereo fills the canvas");

  for (size_t i = 1; i < layout.placements.size(); ++i) {
    Check(layout.placements[i].picture_in_picture,
          "surround tile is an overlay");
    Check(IsInside(layout.placements[i].rect,
                   layout.picture_in_picture_bounds),
          "surround tile stays inside overlay bounds");
  }
  Check(layout.picture_in_picture_bounds.x > 500.0f &&
            layout.picture_in_picture_bounds.y > 300.0f,
        "surround grid is in the bottom-right corner");
}

void TestSurroundLayoutStopsStereoOverlay() {
  const rtc_console::VideoCompositeLayout layout =
      rtc_console::ComputeVideoCompositeLayout(false, 1000.0f, 600.0f,
                                               32.0f / 9.0f);
  Check(layout.placements.size() == 4, "surround layout tile count");
  for (size_t i = 0; i < 4; ++i) {
    Check(!layout.placements[i].picture_in_picture,
          "surround tile is part of the main grid");
  }

    Check(layout.picture_in_picture_bounds.width == 0 &&
      layout.picture_in_picture_bounds.height == 0, "no inactive stereo overlay");
}

void TestLayoutFitsCompactCanvas() {
  const rtc_console::VideoCompositeLayout layout =
      rtc_console::ComputeVideoCompositeLayout(true, 240.0f, 120.0f, 0.0f);
  const rtc_console::VideoRect canvas{0.0f, 0.0f, 240.0f, 120.0f};
  for (const rtc_console::VideoTilePlacement& placement : layout.placements) {
    Check(placement.rect.width > 0.0f && placement.rect.height > 0.0f,
          "compact tile has positive size");
    Check(IsInside(placement.rect, canvas), "compact tile stays in canvas");
  }
}

void TestSurroundPreviewPreservesDrivingView() {
  const float sizes[][2] = {{240.0f, 120.0f}, {640.0f, 360.0f},
           {1280.0f, 720.0f}, {1920.0f, 1080.0f},
           {2560.0f, 720.0f}};
  for (const auto& size : sizes) {
    const auto layout = rtc_console::ComputeVideoCompositeLayout(
    true, size[0], size[1], 32.0f / 9.0f);
    const auto& preview = layout.picture_in_picture_bounds;
    Check(preview.width <= size[0] * 0.28f + 0.01f,
      "surround preview uses at most 28 percent of canvas width");
    Check(preview.height <= size[1] * 0.30f + 0.01f,
      "surround preview uses at most 30 percent of canvas height");
    Check(preview.width <= 384.01f, "surround preview has a desktop size cap");
    Check(NearlyEqual(preview.width / preview.height, 16.0f / 9.0f),
      "surround preview preserves grid aspect ratio");
    Check(NearlyEqual(preview.x + preview.width, size[0] - 8.0f) &&
      NearlyEqual(preview.y + preview.height, size[1] - 8.0f),
      "surround preview remains anchored at bottom right");
  }
}

}  // 匿名命名空间

int main() {
  TestBinocularLayoutUsesSurroundGridOverlay();
  TestSurroundLayoutStopsStereoOverlay();
  TestLayoutFitsCompactCanvas();
  TestSurroundPreviewPreservesDrivingView();
  std::cout << "rtc_console_video_composite_layout_tests passed" << std::endl;
  return 0;
}
