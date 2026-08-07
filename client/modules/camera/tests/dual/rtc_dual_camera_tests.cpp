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

std::vector<uint8_t> MakeUyvyFrame(uint8_t y_base,
                                   uint8_t u_base,
                                   uint8_t v_base) {
  std::vector<uint8_t> frame;
  for (size_t row = 0; row < 2; ++row) {
    frame.push_back(static_cast<uint8_t>(u_base + row * 4));
    frame.push_back(static_cast<uint8_t>(y_base + row * 4));
    frame.push_back(static_cast<uint8_t>(v_base + row * 4));
    frame.push_back(static_cast<uint8_t>(y_base + row * 4 + 1));
    frame.push_back(static_cast<uint8_t>(u_base + row * 4 + 2));
    frame.push_back(static_cast<uint8_t>(y_base + row * 4 + 2));
    frame.push_back(static_cast<uint8_t>(v_base + row * 4 + 2));
    frame.push_back(static_cast<uint8_t>(y_base + row * 4 + 3));
  }
  return frame;
}

void TestDualCudaConversionPipeline() {
  const std::vector<uint8_t> first_left = MakeUyvyFrame(1, 10, 20);
  const std::vector<uint8_t> first_right = MakeUyvyFrame(21, 30, 40);
  const std::vector<uint8_t> second_left = MakeUyvyFrame(41, 50, 60);
  const std::vector<uint8_t> second_right = MakeUyvyFrame(61, 70, 80);
  const std::vector<uint8_t> expected_first = {
      1, 3, 21, 23,
      5, 7, 25, 27,
      13, 33,
      23, 43,
  };
  const std::vector<uint8_t> expected_second = {
      41, 43, 61, 63,
      45, 47, 65, 67,
      53, 73,
      63, 83,
  };

  rtc_camera::dual::ImageFrame first;
  first.format = rtc_camera::dual::ImagePixelFormat::kDualUyvy;
  first.left_pixel_format = V4L2_PIX_FMT_UYVY;
  first.right_pixel_format = V4L2_PIX_FMT_UYVY;
  first.left_width = 4;
  first.left_height = 2;
  first.left_stride_bytes = 8;
  first.left_data = first_left.data();
  first.left_data_size = first_left.size();
  first.right_width = 4;
  first.right_height = 2;
  first.right_stride_bytes = 8;
  first.right_data = first_right.data();
  first.right_data_size = first_right.size();
  rtc_camera::dual::ImageFrame second = first;
  second.left_data = second_left.data();
  second.left_data_size = second_left.size();
  second.right_data = second_right.data();
  second.right_data_size = second_right.size();

  rtc_camera::dual::DualUyvyFrameConverter converter;
  std::string error_message;
  Check(converter.EnqueueToI420(first, &error_message),
        "enqueue first dual CUDA frame: " + error_message);
  Check(converter.EnqueueToI420(second, &error_message),
        "enqueue second dual CUDA frame: " + error_message);
  Check(converter.pending_frames() == 2,
        "two dual CUDA frames are pending");

  rtc_camera::dual::ConvertedI420Frame converted;
  Check(converter.DequeueI420(&converted, &error_message),
        "dequeue first dual CUDA frame: " + error_message);
  Check(std::vector<uint8_t>(converted.data,
                             converted.data + converted.data_size) ==
            expected_first,
        "first dual CUDA pipeline frame bytes");
  Check(converter.DequeueI420(&converted, &error_message),
        "dequeue second dual CUDA frame: " + error_message);
  Check(std::vector<uint8_t>(converted.data,
                             converted.data + converted.data_size) ==
            expected_second,
        "second dual CUDA pipeline frame bytes");
  Check(converter.pending_frames() == 0, "dual CUDA pipeline is empty");
}

}  // 匿名命名空间

int main() {
  TestFrameState();
  TestClosedSubscription();
  TestDualNv12Conversion();
  TestDualCudaConversionPipeline();
  std::cout << "rtc_camera_dual_tests passed" << std::endl;
  return 0;
}
