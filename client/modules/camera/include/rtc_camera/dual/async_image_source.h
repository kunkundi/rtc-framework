#pragma once

#include <stddef.h>
#include <stdint.h>

#include <chrono>
#include <memory>
#include <string>

namespace rtc_camera {
namespace dual {

class AsyncImageFrameSubscription;

enum class ImagePixelFormat {
  kI420,
  kDualUyvy,
};

struct ImageFrame {
  ImagePixelFormat format = ImagePixelFormat::kI420;
  uint64_t sequence = 0;
  int64_t timestamp_us = 0;
  // 消费者完成双路采样和拼接后得到的逻辑输出尺寸。
  size_t width = 0;
  size_t height = 0;

  // I420 帧字段。
  size_t stride_y = 0;
  size_t stride_u = 0;
  size_t stride_v = 0;
  const uint8_t* data = nullptr;
  size_t data_size = 0;

  // 双路 UYVY 帧保持独立存储，由消费者选择具体转换方式。
  uint32_t left_pixel_format = 0;
  uint32_t right_pixel_format = 0;
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

  bool empty() const;

 private:
  friend class AsyncImageFrameSubscription;

  std::shared_ptr<const void> owner_;
};

struct AsyncDualCameraImageSourceOptions {
  std::string left_device = "/dev/video0";
  std::string right_device = "/dev/video1";
  int width = 0;
  int height = 0;
  int buffer_count = 4;
  int timeout_ms = 2000;
  int warmup_frames = 0;
  int warmup_delay_ms = 0;
};

class AsyncImageFrameSubscription {
 public:
  ~AsyncImageFrameSubscription();

  AsyncImageFrameSubscription(const AsyncImageFrameSubscription&) = delete;
  AsyncImageFrameSubscription& operator=(const AsyncImageFrameSubscription&) =
      delete;

  bool WaitNext(ImageFrame* frame, std::chrono::milliseconds timeout);
  void Close();

 private:
  friend class AsyncDualCameraImageSource;

  struct Impl;

  explicit AsyncImageFrameSubscription(const std::shared_ptr<Impl>& impl);

  std::shared_ptr<Impl> impl_;
};

class AsyncDualCameraImageSource {
 public:
  explicit AsyncDualCameraImageSource(
      const AsyncDualCameraImageSourceOptions& options);
  ~AsyncDualCameraImageSource();

  AsyncDualCameraImageSource(const AsyncDualCameraImageSource&) = delete;
  AsyncDualCameraImageSource& operator=(const AsyncDualCameraImageSource&) =
      delete;

  std::shared_ptr<AsyncImageFrameSubscription> Subscribe(
      size_t queue_depth = 2);

  void Start();
  void Stop();

  bool running() const;
  bool failed() const;
  std::string error_message() const;
  uint64_t captured_frames() const;

 private:
  struct Impl;

  std::unique_ptr<Impl> impl_;
};

}  // 命名空间 dual
}  // 命名空间 rtc_camera
