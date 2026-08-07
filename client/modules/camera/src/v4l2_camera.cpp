#include "rtc_camera/v4l2_camera.h"

#include "rtc_logging/rtc_logging.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace rtc_camera {
namespace {

int Xioctl(int fd, unsigned long request, void* arg) {
  int result = 0;
  do {
    result = ioctl(fd, request, arg);
  } while (result == -1 && errno == EINTR);
  return result;
}

bool IsSupportedPixelFormat(uint32_t pixelformat) {
  return pixelformat == V4L2_PIX_FMT_UYVY ||
         pixelformat == V4L2_PIX_FMT_YUYV ||
         pixelformat == V4L2_PIX_FMT_NV12;
}

bool CurrentFormatMatchesRequest(const v4l2_format& current,
                                 const CameraCaptureOptions& options) {
  if (options.width > 0 &&
      current.fmt.pix.width != static_cast<uint32_t>(options.width)) {
    return false;
  }
  if (options.height > 0 &&
      current.fmt.pix.height != static_cast<uint32_t>(options.height)) {
    return false;
  }
  // Jetson 优先使用 YUYV，当前格式是 UYVY 时仍继续协商。
  if (!IsSupportedPixelFormat(current.fmt.pix.pixelformat)) {
    return false;
  }
  // 只有当前格式已经是 YUYV 或 NV12 时才直接使用。
  // 某些驱动默认报告 UYVY，但实际输出可能采用 YUYV 字节顺序。
  if (current.fmt.pix.pixelformat == V4L2_PIX_FMT_YUYV ||
      current.fmt.pix.pixelformat == V4L2_PIX_FMT_NV12) {
    return true;
  }
  // 当前是 UYVY 时继续尝试协商更合适的格式。
  return false;
}

}  // 匿名命名空间

std::string PixelFormatToString(uint32_t pixel_format) {
  char text[5] = {
      static_cast<char>(pixel_format & 0xff),
      static_cast<char>((pixel_format >> 8) & 0xff),
      static_cast<char>((pixel_format >> 16) & 0xff),
      static_cast<char>((pixel_format >> 24) & 0xff),
      '\0'};
  for (int i = 0; i < 4; ++i) {
    if (text[i] == '\0' || text[i] < 32 || text[i] > 126) {
      text[i] = '.';
    }
  }
  return std::string(text);
}

V4l2CameraDevice::~V4l2CameraDevice() {
  Close();
}

void V4l2CameraDevice::Open(const CameraCaptureOptions& options) {
  fd_ = open(options.device.c_str(), O_RDWR | O_NONBLOCK, 0);
  if (fd_ < 0) {
    throw std::runtime_error("failed to open " + options.device + ": " +
                             strerror(errno));
  }
  device_path_ = options.device;

  v4l2_capability caps;
  memset(&caps, 0, sizeof(caps));
  if (Xioctl(fd_, VIDIOC_QUERYCAP, &caps) < 0) {
    throw std::runtime_error("VIDIOC_QUERYCAP failed: " +
                             std::string(strerror(errno)));
  }
  if ((caps.capabilities & V4L2_CAP_VIDEO_CAPTURE) == 0) {
    throw std::runtime_error("device is not a V4L2 capture node");
  }
  if ((caps.capabilities & V4L2_CAP_STREAMING) == 0) {
    throw std::runtime_error("device does not support streaming I/O");
  }

  ConfigureFormat(options);
  InitMmap(options.buffer_count);
  QueueAllBuffers();
  StartStreaming();
}

void V4l2CameraDevice::Close() {
  StopStreaming();
  for (auto& buffer : buffers_) {
    if (buffer.start && buffer.length > 0) {
      munmap(buffer.start, buffer.length);
    }
  }
  buffers_.clear();

  if (fd_ >= 0) {
    close(fd_);
    fd_ = -1;
  }
}

bool V4l2CameraDevice::DequeueCapturedFrame(CapturedFrame* frame) {
  if (fd_ < 0) {
    throw std::runtime_error("capture device is not open");
  }
  if (!frame) {
    throw std::runtime_error("captured frame output is null");
  }

  bool ready = false;
  if (!WaitForCapturedFrames(this, nullptr, &ready, nullptr) || !ready) {
    return false;
  }
  return DequeueReadyCapturedFrame(frame);
}

bool V4l2CameraDevice::DequeueReadyCapturedFrame(CapturedFrame* frame) {
  if (fd_ < 0) {
    throw std::runtime_error("capture device is not open");
  }
  if (!frame) {
    throw std::runtime_error("captured frame output is null");
  }

  v4l2_buffer buffer;
  memset(&buffer, 0, sizeof(buffer));
  buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  buffer.memory = V4L2_MEMORY_MMAP;
  if (Xioctl(fd_, VIDIOC_DQBUF, &buffer) < 0) {
    if (errno == EAGAIN) {
      return false;
    }
    throw std::runtime_error("VIDIOC_DQBUF failed: " +
                             std::string(strerror(errno)));
  }

  if (buffer.index >= buffers_.size()) {
    throw std::runtime_error("driver returned invalid buffer index");
  }

  frame->data = static_cast<const uint8_t*>(buffers_[buffer.index].start);
  frame->bytes_used = buffer.bytesused;
  frame->buffer_index = buffer.index;
  return true;
}

bool WaitForCapturedFrames(V4l2CameraDevice* first,
                           V4l2CameraDevice* second,
                           bool* first_ready,
                           bool* second_ready) {
  if (first_ready) {
    *first_ready = false;
  }
  if (second_ready) {
    *second_ready = false;
  }
  if (!first && !second) {
    return false;
  }

  pollfd descriptors[2];
  V4l2CameraDevice* devices[2] = {first, second};
  size_t descriptor_count = 0;
  int timeout_ms = -1;
  for (size_t i = 0; i < 2; ++i) {
    if (!devices[i]) {
      continue;
    }
    if (devices[i]->fd_ < 0) {
      throw std::runtime_error("capture device is not open");
    }
    descriptors[descriptor_count].fd = devices[i]->fd_;
    descriptors[descriptor_count].events = POLLIN;
    descriptors[descriptor_count].revents = 0;
    if (timeout_ms < 0 || devices[i]->timeout_ms_ < timeout_ms) {
      timeout_ms = devices[i]->timeout_ms_;
    }
    ++descriptor_count;
  }

  const int poll_ret = poll(descriptors, descriptor_count, timeout_ms);
  if (poll_ret == 0) {
    return false;
  }
  if (poll_ret < 0) {
    if (errno == EINTR) {
      return false;
    }
    throw std::runtime_error("poll failed: " + std::string(strerror(errno)));
  }

  size_t descriptor_index = 0;
  bool any_ready = false;
  for (size_t i = 0; i < 2; ++i) {
    if (!devices[i]) {
      continue;
    }
    const short revents = descriptors[descriptor_index++].revents;
    if ((revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
      throw std::runtime_error("camera poll reported a device error");
    }
    const bool ready = (revents & (POLLIN | POLLPRI)) != 0;
    if (i == 0 && first_ready) {
      *first_ready = ready;
    }
    if (i == 1 && second_ready) {
      *second_ready = ready;
    }
    any_ready = any_ready || ready;
  }
  return any_ready;
}

void V4l2CameraDevice::RequeueCapturedFrame(CapturedFrame* frame) {
  if (!frame || !frame->data) {
    return;
  }
  if (fd_ < 0) {
    throw std::runtime_error("capture device is not open");
  }
  if (frame->buffer_index >= buffers_.size()) {
    throw std::runtime_error("captured frame has invalid buffer index");
  }

  v4l2_buffer buffer;
  memset(&buffer, 0, sizeof(buffer));
  buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  buffer.memory = V4L2_MEMORY_MMAP;
  buffer.index = frame->buffer_index;
  if (Xioctl(fd_, VIDIOC_QBUF, &buffer) < 0) {
    throw std::runtime_error("VIDIOC_QBUF failed: " +
                             std::string(strerror(errno)));
  }

  frame->data = nullptr;
  frame->bytes_used = 0;
  frame->buffer_index = 0;
}

uint32_t V4l2CameraDevice::pixel_format() const {
  return format_.fmt.pix.pixelformat;
}

uint32_t V4l2CameraDevice::width() const {
  return format_.fmt.pix.width;
}

uint32_t V4l2CameraDevice::height() const {
  return format_.fmt.pix.height;
}

size_t V4l2CameraDevice::bytes_per_line() const {
  if (format_.fmt.pix.bytesperline) {
    return format_.fmt.pix.bytesperline;
  }
  if (format_.fmt.pix.pixelformat == V4L2_PIX_FMT_NV12) {
    return static_cast<size_t>(width());
  }
  return static_cast<size_t>(width()) * 2;
}

bool V4l2CameraDevice::is_nv12() const {
  return format_.fmt.pix.pixelformat == V4L2_PIX_FMT_NV12;
}

const std::string& V4l2CameraDevice::device_path() const {
  return device_path_;
}

void V4l2CameraDevice::ConfigureFormat(const CameraCaptureOptions& options) {
  timeout_ms_ = options.timeout_ms;

  memset(&format_, 0, sizeof(format_));
  format_.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (Xioctl(fd_, VIDIOC_G_FMT, &format_) < 0) {
    throw std::runtime_error("VIDIOC_G_FMT failed: " +
                             std::string(strerror(errno)));
  }

  if (CurrentFormatMatchesRequest(format_, options)) {
    rtc_logging::LogInfo(std::string("camera ") + options.device + " using native format " +
            PixelFormatToString(format_.fmt.pix.pixelformat) + " " +
            std::to_string(format_.fmt.pix.width) + "x" +
            std::to_string(format_.fmt.pix.height));
    return;
  }

  v4l2_format desired = format_;
  desired.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (options.width > 0) {
    desired.fmt.pix.width = static_cast<uint32_t>(options.width);
  }
  if (options.height > 0) {
    desired.fmt.pix.height = static_cast<uint32_t>(options.height);
  }
  desired.fmt.pix.field = V4L2_FIELD_ANY;

  // 按 Jetson 常用顺序尝试格式：YUYV、UYVY、NV12。
  static const uint32_t kPreferredFormats[] = {
      V4L2_PIX_FMT_YUYV,
      V4L2_PIX_FMT_UYVY,
      V4L2_PIX_FMT_NV12,
  };

  bool format_set = false;
  for (uint32_t fmt : kPreferredFormats) {
    desired.fmt.pix.pixelformat = fmt;
    if (Xioctl(fd_, VIDIOC_S_FMT, &desired) == 0 &&
        desired.fmt.pix.pixelformat == fmt) {
      format_ = desired;
      format_set = true;
      rtc_logging::LogInfo(std::string("camera ") + options.device + " set to " +
              PixelFormatToString(fmt) + " " +
              std::to_string(format_.fmt.pix.width) + "x" +
              std::to_string(format_.fmt.pix.height));
      if ((options.width > 0 &&
           format_.fmt.pix.width != static_cast<uint32_t>(options.width)) ||
          (options.height > 0 &&
           format_.fmt.pix.height != static_cast<uint32_t>(options.height))) {
        rtc_logging::LogInfo(
            std::string("camera ") + options.device +
            " adjusted requested size " + std::to_string(options.width) + "x" +
            std::to_string(options.height) + " to " +
            std::to_string(format_.fmt.pix.width) + "x" +
            std::to_string(format_.fmt.pix.height));
      }
      break;
    }
  }

  if (!format_set) {
    throw std::runtime_error(
        "unable to apply the requested pixel format on " + options.device +
        " (tried UYVY, YUYV, NV12)");
  }
}

void V4l2CameraDevice::InitMmap(int requested_count) {
  v4l2_requestbuffers req;
  memset(&req, 0, sizeof(req));
  req.count = static_cast<uint32_t>(requested_count);
  req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  req.memory = V4L2_MEMORY_MMAP;
  if (Xioctl(fd_, VIDIOC_REQBUFS, &req) < 0) {
    throw std::runtime_error("VIDIOC_REQBUFS failed: " +
                             std::string(strerror(errno)));
  }
  if (req.count < 2) {
    throw std::runtime_error("not enough mmap buffers returned by driver");
  }

  buffers_.resize(req.count);
  for (uint32_t i = 0; i < req.count; ++i) {
    v4l2_buffer buffer;
    memset(&buffer, 0, sizeof(buffer));
    buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buffer.memory = V4L2_MEMORY_MMAP;
    buffer.index = i;
    if (Xioctl(fd_, VIDIOC_QUERYBUF, &buffer) < 0) {
      throw std::runtime_error("VIDIOC_QUERYBUF failed: " +
                               std::string(strerror(errno)));
    }

    buffers_[i].length = buffer.length;
    buffers_[i].start = mmap(nullptr, buffer.length, PROT_READ | PROT_WRITE,
                             MAP_SHARED, fd_, buffer.m.offset);
    if (buffers_[i].start == MAP_FAILED) {
      buffers_[i].start = nullptr;
      throw std::runtime_error("mmap failed: " + std::string(strerror(errno)));
    }
  }
}

void V4l2CameraDevice::QueueAllBuffers() {
  for (uint32_t i = 0; i < buffers_.size(); ++i) {
    v4l2_buffer buffer;
    memset(&buffer, 0, sizeof(buffer));
    buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buffer.memory = V4L2_MEMORY_MMAP;
    buffer.index = i;
    if (Xioctl(fd_, VIDIOC_QBUF, &buffer) < 0) {
      throw std::runtime_error("VIDIOC_QBUF failed while priming buffers: " +
                               std::string(strerror(errno)));
    }
  }
}

void V4l2CameraDevice::StartStreaming() {
  v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (Xioctl(fd_, VIDIOC_STREAMON, &type) < 0) {
    throw std::runtime_error("VIDIOC_STREAMON failed: " +
                             std::string(strerror(errno)));
  }
  streaming_started_ = true;
}

void V4l2CameraDevice::StopStreaming() {
  if (fd_ >= 0 && streaming_started_) {
    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    Xioctl(fd_, VIDIOC_STREAMOFF, &type);
    streaming_started_ = false;
  }
}

void RunWarmup(V4l2CameraDevice& device, const CameraCaptureOptions& options) {
  if (options.warmup_delay_ms > 0) {
    std::this_thread::sleep_for(
        std::chrono::milliseconds(options.warmup_delay_ms));
  }
  if (options.warmup_frames <= 0) {
    return;
  }

  V4l2CameraDevice::CapturedFrame discard_frame;
  int discarded = 0;
  while (discarded < options.warmup_frames) {
    if (!device.DequeueCapturedFrame(&discard_frame)) {
      continue;
    }
    device.RequeueCapturedFrame(&discard_frame);
    ++discarded;
  }
}

}  // 命名空间 rtc_camera
