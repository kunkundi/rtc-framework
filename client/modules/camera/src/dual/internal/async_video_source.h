#pragma once

#include "rtc_camera/camera_capture_options.h"
#include "rtc_camera/v4l2_camera.h"

#include <stddef.h>
#include <stdint.h>

#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace rtc_camera {
namespace dual {
namespace internal {

enum class VideoFrameFormat {
  kI420,
  kDualUyvy,
};

struct VideoFrame {
  ~VideoFrame();

  VideoFrameFormat format = VideoFrameFormat::kDualUyvy;
  uint32_t left_pixel_format = 0;
  uint32_t right_pixel_format = 0;
  uint64_t sequence = 0;
  int64_t timestamp_us = 0;
  // 消费者完成左右采样拼接后得到的逻辑输出尺寸。
  size_t width = 0;
  size_t height = 0;

  // I420 帧字段。
  size_t stride_y = 0;
  size_t stride_u = 0;
  size_t stride_v = 0;
  std::vector<uint8_t> data;

  // 双路 UYVY 帧字段。
  size_t left_width = 0;
  size_t left_height = 0;
  size_t left_stride_bytes = 0;
  const uint8_t* left_data = nullptr;
  size_t left_data_size = 0;
  size_t right_width = 0;
  size_t right_height = 0;
  size_t right_stride_bytes = 0;
  const uint8_t* right_data = nullptr;
  size_t right_data_size = 0;

  // 保持 mmap 缓冲及其设备有效，最后一个消费者释放后自动回队。
  std::shared_ptr<rtc_camera::V4l2CameraDevice> left_device;
  std::shared_ptr<rtc_camera::V4l2CameraDevice> right_device;
  rtc_camera::V4l2CameraDevice::CapturedFrame left_captured_frame;
  rtc_camera::V4l2CameraDevice::CapturedFrame right_captured_frame;
};

using VideoFramePtr = std::shared_ptr<const VideoFrame>;

class VideoSourceSubscription {
 public:
  explicit VideoSourceSubscription(size_t queue_depth);
  ~VideoSourceSubscription();

  VideoSourceSubscription(const VideoSourceSubscription&) = delete;
  VideoSourceSubscription& operator=(const VideoSourceSubscription&) = delete;

  bool WaitNext(VideoFramePtr* frame, std::chrono::milliseconds timeout);
  void Close();

 private:
  friend class AsyncDualCameraVideoSource;

  void Push(VideoFramePtr frame);

  const size_t queue_depth_;
  bool closed_ = false;
  std::mutex mutex_;
  std::condition_variable condition_;
  std::deque<VideoFramePtr> queue_;
};

struct DualCameraVideoSourceConfig {
  rtc_camera::CameraCaptureOptions options;
  std::string left_device;
  std::string right_device;
};

class AsyncDualCameraVideoSource {
 public:
  explicit AsyncDualCameraVideoSource(
      const DualCameraVideoSourceConfig& config);
  ~AsyncDualCameraVideoSource();

  AsyncDualCameraVideoSource(const AsyncDualCameraVideoSource&) = delete;
  AsyncDualCameraVideoSource& operator=(const AsyncDualCameraVideoSource&) =
      delete;

  std::shared_ptr<VideoSourceSubscription> Subscribe(size_t queue_depth = 2);

  void Start();
  void Stop();

  bool running() const;
  bool failed() const;
  std::string error_message() const;
  uint64_t captured_frames() const;

 private:
  void CaptureLoop();
  void Publish(VideoFramePtr frame);
  void CloseSubscriptions();
  void SetError(const std::string& error_message);

  DualCameraVideoSourceConfig config_;
  mutable std::mutex state_mutex_;
  bool started_ = false;
  bool stop_requested_ = false;
  bool failed_ = false;
  std::string error_message_;
  uint64_t captured_frames_ = 0;
  std::thread capture_thread_;
  std::vector<std::weak_ptr<VideoSourceSubscription>> subscriptions_;
};

}  // 命名空间 internal
}  // 命名空间 dual
}  // 命名空间 rtc_camera
