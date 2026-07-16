#include "rtc_camera/dual/async_image_source.h"
#include "rtc_camera/dual/frame_converter.h"

#include <linux/videodev2.h>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

void Check(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << std::endl;
    std::exit(1);
  }
}

void TestFrameState() {
  rtc_camera::dual::ImageFrame frame;
  Check(frame.empty(), "default dual-camera frame is empty");
}

void TestClosedSubscription() {
  rtc_camera::dual::AsyncDualCameraImageSourceOptions options;
  options.left_device = "/dev/video-left-test";
  options.right_device = "/dev/video-right-test";
  rtc_camera::dual::AsyncDualCameraImageSource source(options);
  std::shared_ptr<rtc_camera::dual::AsyncImageFrameSubscription> subscription =
      source.Subscribe(1);
  Check(static_cast<bool>(subscription), "create dual-camera subscription");

  subscription->Close();
  rtc_camera::dual::ImageFrame frame;
  Check(!subscription->WaitNext(&frame, std::chrono::milliseconds(0)),
        "closed dual-camera subscription has no frame");
  Check(!source.running(), "dual-camera source is stopped by default");
  Check(source.captured_frames() == 0,
        "dual-camera source has no captured frames");
}

void TestDualNv12Conversion() {
  const std::vector<uint8_t> left_nv12 = {1, 2, 3, 4, 5, 6};
  const std::vector<uint8_t> right_nv12 = {7, 8, 9, 10, 11, 12};
  const std::vector<uint8_t> expected_i420 = {
      1, 2, 7, 8,
      3, 4, 9, 10,
      5, 11,
      6, 12,
  };

  rtc_camera::dual::ImageFrame frame;
  frame.format = rtc_camera::dual::ImagePixelFormat::kDualUyvy;
  frame.left_pixel_format = V4L2_PIX_FMT_NV12;
  frame.right_pixel_format = V4L2_PIX_FMT_NV12;
  frame.left_width = 2;
  frame.left_height = 2;
  frame.left_stride_bytes = 2;
  frame.left_data = left_nv12.data();
  frame.left_data_size = left_nv12.size();
  frame.right_width = 2;
  frame.right_height = 2;
  frame.right_stride_bytes = 2;
  frame.right_data = right_nv12.data();
  frame.right_data_size = right_nv12.size();

  rtc_camera::dual::DualUyvyFrameConverter converter;
  rtc_camera::dual::ConvertedI420Frame converted;
  std::string error_message;
  Check(converter.ConvertToI420(frame, &converted, &error_message),
        "convert dual NV12 frame: " + error_message);
  Check(converted.width == 4 && converted.height == 2,
        "converted dual frame dimensions");
  Check(converted.stride_y == 4 && converted.stride_u == 2 &&
            converted.stride_v == 2,
        "converted dual frame strides");
  Check(converted.data_size == expected_i420.size(),
        "converted dual frame size");
  Check(std::vector<uint8_t>(converted.data,
                             converted.data + converted.data_size) ==
            expected_i420,
        "converted dual frame bytes");
}

}  // 匿名命名空间

int main() {
  TestFrameState();
  TestClosedSubscription();
  TestDualNv12Conversion();
  std::cout << "rtc_camera_dual_tests passed" << std::endl;
  return 0;
}
