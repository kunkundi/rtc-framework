#include "video_detection_overlay.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>

namespace {

using rtc_console::MapDetectionRectToImage;
using rtc_console::NormalizedRect;
using rtc_console::VideoRect;

void Check(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << std::endl;
    std::exit(1);
  }
}

void CheckRect(const VideoRect& actual, const VideoRect& expected,
               const std::string& message) {
  Check(std::fabs(actual.x - expected.x) < 0.01f &&
        std::fabs(actual.y - expected.y) < 0.01f &&
        std::fabs(actual.width - expected.width) < 0.01f &&
        std::fabs(actual.height - expected.height) < 0.01f, message);
}

void TestSurroundGridCrops() {
  const NormalizedRect crops[] = {
      {0, 0, 0.5f, 0.5f}, {0.5f, 0, 1, 0.5f},
      {0, 0.5f, 0.5f, 1}, {0.5f, 0.5f, 1, 1}};
  // 四宫格中每路的框占该路宽度的 30%、高度的 40%。
  const NormalizedRect boxes[] = {
      {0.05f, 0.10f, 0.20f, 0.30f}, {0.55f, 0.10f, 0.70f, 0.30f},
      {0.05f, 0.60f, 0.20f, 0.80f}, {0.55f, 0.60f, 0.70f, 0.80f}};
  const VideoRect images[] = {
      {700, 450, 160, 90}, {150, 200, 640, 360}, {0, 60, 1920, 1080}};
  const VideoRect expected[] = {
      {716, 468, 48, 36}, {214, 272, 192, 144}, {192, 276, 576, 432}};
  for (int source = 0; source < 4; ++source) {
    for (int view = 0; view < 3; ++view) {
      VideoRect mapped;
      Check(MapDetectionRectToImage(boxes[source], crops[source], images[view], &mapped),
            "show detection in its surround tile");
      CheckRect(mapped, expected[view], "preserve box proportions in preview, main and fullscreen views");
      for (int other = 0; other < 4; ++other) {
        if (other == source) continue;
        Check(!MapDetectionRectToImage(boxes[source], crops[other], images[view], &mapped),
              "do not repeat detections in other surround tiles");
      }
    }
  }
}

void TestCropBoundaryClipping() {
  VideoRect mapped;
  const VideoRect image{10, 20, 160, 90};
  const NormalizedRect crop{0.5f, 0.5f, 1, 1};
  Check(MapDetectionRectToImage({0.4f, 0.4f, 0.6f, 0.6f}, crop, image, &mapped),
        "clip a partially visible detection");
  CheckRect(mapped, {10, 20, 32, 18}, "clip at the displayed crop boundary");
  Check(MapDetectionRectToImage({0, 0, 1, 1}, crop, image, &mapped),
        "clip a detection enclosing the crop");
  CheckRect(mapped, image, "enclosing detection fills only the displayed image");
  Check(!MapDetectionRectToImage({0, 0, 0.5f, 0.5f}, crop, image, &mapped),
        "touching crop edges does not create an empty box");
}

void TestFullFrameAndImageOffsets() {
  VideoRect mapped;
  const NormalizedRect full{0, 0, 1, 1};
  Check(MapDetectionRectToImage({0.1f, 0.2f, 0.4f, 0.6f}, full,
                                {30, 50, 640, 360}, &mapped),
        "map an uncropped stereo or single-camera frame");
  CheckRect(mapped, {94, 122, 192, 144}, "use image offset after letterboxing and titles");
  Check(MapDetectionRectToImage({0.55f, 0.6f, 0.7f, 0.8f},
                                {0.5f, 0.5f, 1, 1}, {100, 150, 160, 90}, &mapped),
        "map a dragged preview");
  CheckRect(mapped, {116, 168, 48, 36}, "dragging translates boxes without resizing");
}

void TestInvalidGeometry() {
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const NormalizedRect full{0, 0, 1, 1};
  const VideoRect image{0, 0, 160, 90};
  VideoRect mapped;
  for (const auto& crop : {NormalizedRect{}, NormalizedRect{0.5f, 0, 0.5f, 1},
                           NormalizedRect{1, 0, 0, 1}, NormalizedRect{-1, 0, 1, 1},
                           NormalizedRect{0, 0, 1, 2}, NormalizedRect{nan, 0, 1, 1}}) {
    Check(!MapDetectionRectToImage(full, crop, image, &mapped), "reject invalid crop");
  }
  Check(!MapDetectionRectToImage({0, 0, nan, 1}, full, image, &mapped), "reject non-finite detection");
  Check(!MapDetectionRectToImage(full, full, {0, 0, 0, 90}, &mapped), "reject empty image");
  Check(!MapDetectionRectToImage(full, full, image, nullptr), "reject missing output");
}

}  // 匿名命名空间

int main() {
  TestSurroundGridCrops();
  TestCropBoundaryClipping();
  TestFullFrameAndImageOffsets();
  TestInvalidGeometry();
  std::cout << "rtc_console_video_detection_overlay_tests passed" << std::endl;
  return 0;
}
