#pragma once

#include "rtc_camera/camera_capture_options.h"

#include <stddef.h>
#include <stdint.h>

#include <chrono>
#include <memory>
#include <string>

namespace rtc_camera {

struct CameraFrame {
  uint64_t sequence = 0;
  int64_t timestamp_us = 0;
  uint32_t pixel_format = 0;
  size_t width = 0;
  size_t height = 0;
  size_t stride_bytes = 0;
  const uint8_t* data = nullptr;
  size_t data_size = 0;

  bool empty() const;

 private:
  friend class CameraFrameSubscription;

  // 保持帧数据有效，直到当前 CameraFrame 被释放或被下一帧覆盖。
  std::shared_ptr<const void> owner_;
};

class CameraFrameSubscription {
 public:
  ~CameraFrameSubscription();

  CameraFrameSubscription(const CameraFrameSubscription&) = delete;
  CameraFrameSubscription& operator=(const CameraFrameSubscription&) = delete;

  bool WaitNext(CameraFrame* frame, std::chrono::milliseconds timeout);
  void Close();

 private:
  friend class AsyncCameraImageSource;

  struct Impl;

  explicit CameraFrameSubscription(const std::shared_ptr<Impl>& impl);

  std::shared_ptr<Impl> impl_;
};

class AsyncCameraImageSource {
 public:
  explicit AsyncCameraImageSource(const CameraCaptureOptions& options);
  ~AsyncCameraImageSource();

  AsyncCameraImageSource(const AsyncCameraImageSource&) = delete;
  AsyncCameraImageSource& operator=(const AsyncCameraImageSource&) = delete;

  std::shared_ptr<CameraFrameSubscription> Subscribe(size_t queue_depth = 2);

  void Start();
  void RequestStop();
  void Stop();

  bool running() const;
  bool failed() const;
  std::string error_message() const;
  uint64_t captured_frames() const;

 private:
  struct Impl;

  std::unique_ptr<Impl> impl_;
};

}  // 命名空间 rtc_camera
