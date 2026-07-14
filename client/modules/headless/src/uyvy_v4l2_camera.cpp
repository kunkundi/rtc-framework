#include "rtc_headless/uyvy_v4l2_camera.h"

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

namespace rtc_camera_headless {
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
                                 const CaptureOptions& options) {
  if (options.width > 0 &&
      current.fmt.pix.width != static_cast<uint32_t>(options.width)) {
    return false;
  }
  if (options.height > 0 &&
      current.fmt.pix.height != static_cast<uint32_t>(options.height)) {
    return false;
  }
  // Prefer YUYV on Jetson; never short-circuit on UYVY alone.
  if (!IsSupportedPixelFormat(current.fmt.pix.pixelformat)) {
    return false;
  }
  // Only accept the current format as-is if it's already YUYV or NV12.
  // UYVY may be a driver default that actually outputs YUYV byte order.
  if (current.fmt.pix.pixelformat == V4L2_PIX_FMT_YUYV ||
      current.fmt.pix.pixelformat == V4L2_PIX_FMT_NV12) {
    return true;
  }
  // UYVY: still try to negotiate a better format.
  return false;
}

}  // namespace

UyvyV4l2CaptureDevice::~UyvyV4l2CaptureDevice() {
  Close();
}

void UyvyV4l2CaptureDevice::Open(const CaptureOptions& options) {
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

void UyvyV4l2CaptureDevice::Close() {
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

bool UyvyV4l2CaptureDevice::DequeueCapturedFrame(CapturedFrame* frame) {
  if (fd_ < 0) {
    throw std::runtime_error("capture device is not open");
  }
  if (!frame) {
    throw std::runtime_error("captured frame output is null");
  }

  pollfd pfd;
  pfd.fd = fd_;
  pfd.events = POLLIN;
  pfd.revents = 0;
  const int poll_ret = poll(&pfd, 1, timeout_ms_);
  if (poll_ret == 0) {
    return false;
  }
  if (poll_ret < 0) {
    if (errno == EINTR) {
      return false;
    }
    throw std::runtime_error("poll failed: " + std::string(strerror(errno)));
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

void UyvyV4l2CaptureDevice::RequeueCapturedFrame(CapturedFrame* frame) {
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

uint32_t UyvyV4l2CaptureDevice::pixel_format() const {
  return format_.fmt.pix.pixelformat;
}

uint32_t UyvyV4l2CaptureDevice::width() const {
  return format_.fmt.pix.width;
}

uint32_t UyvyV4l2CaptureDevice::height() const {
  return format_.fmt.pix.height;
}

size_t UyvyV4l2CaptureDevice::bytes_per_line() const {
  if (format_.fmt.pix.bytesperline) {
    return format_.fmt.pix.bytesperline;
  }
  if (format_.fmt.pix.pixelformat == V4L2_PIX_FMT_NV12) {
    return static_cast<size_t>(width());
  }
  return static_cast<size_t>(width()) * 2;
}

bool UyvyV4l2CaptureDevice::is_nv12() const {
  return format_.fmt.pix.pixelformat == V4L2_PIX_FMT_NV12;
}

const std::string& UyvyV4l2CaptureDevice::device_path() const {
  return device_path_;
}

void UyvyV4l2CaptureDevice::ConfigureFormat(const CaptureOptions& options) {
  timeout_ms_ = options.timeout_ms;

  memset(&format_, 0, sizeof(format_));
  format_.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (Xioctl(fd_, VIDIOC_G_FMT, &format_) < 0) {
    throw std::runtime_error("VIDIOC_G_FMT failed: " +
                             std::string(strerror(errno)));
  }

  if (CurrentFormatMatchesRequest(format_, options)) {
    LogInfo(std::string("camera ") + options.device + " using native format " +
            FourccToString(format_.fmt.pix.pixelformat) + " " +
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

  // Try formats in preference order: YUYV (Jetson V4L2 default), UYVY, NV12
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
      LogInfo(std::string("camera ") + options.device + " set to " +
              FourccToString(fmt) + " " +
              std::to_string(format_.fmt.pix.width) + "x" +
              std::to_string(format_.fmt.pix.height));
      break;
    }
  }

  if (!format_set) {
    throw std::runtime_error(
        "unable to apply the requested pixel format on " + options.device +
        " (tried UYVY, YUYV, NV12)");
  }
}

void UyvyV4l2CaptureDevice::InitMmap(int requested_count) {
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

void UyvyV4l2CaptureDevice::QueueAllBuffers() {
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

void UyvyV4l2CaptureDevice::StartStreaming() {
  v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (Xioctl(fd_, VIDIOC_STREAMON, &type) < 0) {
    throw std::runtime_error("VIDIOC_STREAMON failed: " +
                             std::string(strerror(errno)));
  }
  streaming_started_ = true;
}

void UyvyV4l2CaptureDevice::StopStreaming() {
  if (fd_ >= 0 && streaming_started_) {
    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    Xioctl(fd_, VIDIOC_STREAMOFF, &type);
    streaming_started_ = false;
  }
}

void RunWarmup(UyvyV4l2CaptureDevice& device, const CaptureOptions& options) {
  if (options.warmup_delay_ms > 0) {
    std::this_thread::sleep_for(
        std::chrono::milliseconds(options.warmup_delay_ms));
  }
  if (options.warmup_frames <= 0) {
    return;
  }

  UyvyV4l2CaptureDevice::CapturedFrame discard_frame;
  int discarded = 0;
  while (!StopRequested() && discarded < options.warmup_frames) {
    if (!device.DequeueCapturedFrame(&discard_frame)) {
      continue;
    }
    device.RequeueCapturedFrame(&discard_frame);
    ++discarded;
  }
}

}  // namespace rtc_camera_headless
