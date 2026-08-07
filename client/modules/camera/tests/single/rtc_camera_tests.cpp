#include "rtc_camera/single/async_image_source.h"
#include "rtc_camera/single/frame_converter.h"

#include <linux/videodev2.h>

#include <cstdlib>
#include <iostream>
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
  rtc_camera::single::CameraFrame frame;
  Check(frame.empty(), "default frame is empty");

  const uint8_t data[] = {1, 2, 3, 4};
  frame.data = data;
  frame.data_size = sizeof(data);
  Check(!frame.empty(), "frame with data is not empty");
}

void TestClosedSubscription() {
  rtc_camera::CameraCaptureOptions options;
  options.device = "/dev/video-test";
  rtc_camera::single::AsyncCameraImageSource source(options);
  std::shared_ptr<rtc_camera::single::CameraFrameSubscription> subscription =
      source.Subscribe(1);
  Check(static_cast<bool>(subscription), "create subscription");

  subscription->Close();
  rtc_camera::single::CameraFrame frame;
  Check(!subscription->WaitNext(&frame, std::chrono::milliseconds(0)),
        "closed subscription has no frame");
  Check(!source.running(), "source is stopped by default");
  Check(source.captured_frames() == 0, "source has no captured frames");
}

void TestNv12Conversion() {
  const std::vector<uint8_t> nv12 = {
      1, 2, 3, 4,
      5, 6, 7, 8,
      9, 10, 11, 12,
  };
  const std::vector<uint8_t> expected_i420 = {
      1, 2, 3, 4,
      5, 6, 7, 8,
      9, 11,
      10, 12,
  };

  rtc_camera::single::CameraFrame frame;
  frame.pixel_format = V4L2_PIX_FMT_NV12;
  frame.width = 4;
  frame.height = 2;
  frame.stride_bytes = 4;
  frame.data = nv12.data();
  frame.data_size = nv12.size();

  rtc_camera::single::CameraFrameConverter converter;
  rtc_camera::single::ConvertedCameraFrame converted;
  std::string error_message;
  Check(converter.ConvertToI420(frame, &converted, &error_message),
        "convert NV12 frame: " + error_message);
  Check(converted.width == 4 && converted.height == 2,
        "converted frame dimensions");
  Check(converted.stride_y == 4 && converted.stride_u == 2 &&
            converted.stride_v == 2,
        "converted frame strides");
  Check(converted.data_size == expected_i420.size(),
        "converted frame size");
  Check(std::vector<uint8_t>(converted.data,
                             converted.data + converted.data_size) ==
            expected_i420,
        "converted frame bytes");
}

void TestCudaConversionPipeline() {
  const std::vector<uint8_t> first_uyvy = {
      10, 1, 20, 2,
      14, 3, 24, 4,
  };
  const std::vector<uint8_t> second_uyvy = {
      30, 5, 40, 6,
      34, 7, 44, 8,
  };
  const std::vector<uint8_t> expected_first = {1, 2, 3, 4, 12, 22};
  const std::vector<uint8_t> expected_second = {5, 6, 7, 8, 32, 42};

  rtc_camera::single::CameraFrame first;
  first.pixel_format = V4L2_PIX_FMT_UYVY;
  first.width = 2;
  first.height = 2;
  first.stride_bytes = 4;
  first.data = first_uyvy.data();
  first.data_size = first_uyvy.size();
  rtc_camera::single::CameraFrame second = first;
  second.data = second_uyvy.data();
  second.data_size = second_uyvy.size();

  rtc_camera::single::CameraFrameConverter converter;
  std::string error_message;
  Check(converter.EnqueueToI420(first, &error_message),
        "enqueue first CUDA frame: " + error_message);
  Check(converter.EnqueueToI420(second, &error_message),
        "enqueue second CUDA frame: " + error_message);
  Check(converter.pending_frames() == 2, "two CUDA frames are pending");

  rtc_camera::single::ConvertedCameraFrame converted;
  Check(converter.DequeueI420(&converted, &error_message),
        "dequeue first CUDA frame: " + error_message);
  Check(std::vector<uint8_t>(converted.data,
                             converted.data + converted.data_size) ==
            expected_first,
        "first CUDA pipeline frame bytes");
  Check(converter.DequeueI420(&converted, &error_message),
        "dequeue second CUDA frame: " + error_message);
  Check(std::vector<uint8_t>(converted.data,
                             converted.data + converted.data_size) ==
            expected_second,
        "second CUDA pipeline frame bytes");
  Check(converter.pending_frames() == 0, "CUDA pipeline is empty");
}

}  // 匿名命名空间

int main() {
  TestFrameState();
  TestClosedSubscription();
  TestNv12Conversion();
  TestCudaConversionPipeline();
  std::cout << "rtc_camera_tests passed" << std::endl;
  return 0;
}
