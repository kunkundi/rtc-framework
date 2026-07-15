#pragma once

#include "rtc_camera/camera_capture_options.h"

#include <linux/videodev2.h>
#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

namespace rtc_camera {

class V4l2CameraDevice {
 public:
  struct CapturedFrame {
    const uint8_t* data = nullptr;
    size_t bytes_used = 0;
    uint32_t buffer_index = 0;
  };

  ~V4l2CameraDevice();

  void Open(const CameraCaptureOptions& options);
  void Close();

  bool DequeueCapturedFrame(CapturedFrame* frame);
  void RequeueCapturedFrame(CapturedFrame* frame);

  uint32_t pixel_format() const;
  bool is_nv12() const;
  uint32_t width() const;
  uint32_t height() const;
  size_t bytes_per_line() const;
  const std::string& device_path() const;

 private:
  void ConfigureFormat(const CameraCaptureOptions& options);
  void InitMmap(int requested_count);
  void QueueAllBuffers();
  void StartStreaming();
  void StopStreaming();

  struct MappedBuffer {
    void* start = nullptr;
    size_t length = 0;
  };

  int fd_ = -1;
  int timeout_ms_ = 2000;
  bool streaming_started_ = false;
  std::string device_path_;
  v4l2_format format_{};
  std::vector<MappedBuffer> buffers_;
};

void RunWarmup(V4l2CameraDevice& device,
               const CameraCaptureOptions& options);
std::string PixelFormatToString(uint32_t pixel_format);

}  // 命名空间 rtc_camera
