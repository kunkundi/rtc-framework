#include "async_dual_camera_video_source.h"

#include "rtc_camera/v4l2_camera.h"
#include "rtc_logging/rtc_logging.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <stdexcept>

namespace rtc_dual_camera {
namespace internal {
namespace {

class CapturedFrameGuard {
 public:
  CapturedFrameGuard(rtc_camera::V4l2CameraDevice* device,
                     rtc_camera::V4l2CameraDevice::CapturedFrame* frame)
      : device_(device), frame_(frame) {}

  ~CapturedFrameGuard() {
    if (device_ && frame_ && frame_->data) {
      device_->RequeueCapturedFrame(frame_);
    }
  }

  CapturedFrameGuard(const CapturedFrameGuard&) = delete;
  CapturedFrameGuard& operator=(const CapturedFrameGuard&) = delete;

 private:
  rtc_camera::V4l2CameraDevice* device_ = nullptr;
  rtc_camera::V4l2CameraDevice::CapturedFrame* frame_ = nullptr;
};

int64_t NowMicros() {
  const auto now = std::chrono::steady_clock::now();
  return std::chrono::duration_cast<std::chrono::microseconds>(
             now.time_since_epoch())
      .count();
}

void LogCameraInfo(const char* label, const rtc_camera::V4l2CameraDevice& device) {
  rtc_logging::LogInfo(std::string(label) + ": " + device.device_path() + " " +
          std::to_string(device.width()) + "x" +
          std::to_string(device.height()) + " " +
          rtc_camera::PixelFormatToString(device.pixel_format()));
}

bool StopRequestedBySource(const AsyncDualCameraVideoSource* source) {
  return !source->running();
}

void RunDualWarmup(rtc_camera::V4l2CameraDevice& left_device,
                   rtc_camera::V4l2CameraDevice& right_device,
                   const rtc_camera::CameraCaptureOptions& options,
                   const AsyncDualCameraVideoSource* source) {
  if (options.warmup_delay_ms > 0) {
    std::this_thread::sleep_for(
        std::chrono::milliseconds(options.warmup_delay_ms));
  }
  if (options.warmup_frames <= 0) {
    return;
  }

  int discarded = 0;
  while (!StopRequestedBySource(source) &&
         discarded < options.warmup_frames) {
    rtc_camera::V4l2CameraDevice::CapturedFrame left_frame;
    if (!left_device.DequeueCapturedFrame(&left_frame)) {
      continue;
    }
    CapturedFrameGuard left_guard(&left_device, &left_frame);

    rtc_camera::V4l2CameraDevice::CapturedFrame right_frame;
    if (!right_device.DequeueCapturedFrame(&right_frame)) {
      continue;
    }
    CapturedFrameGuard right_guard(&right_device, &right_frame);
    ++discarded;
  }
}

}  // 匿名命名空间

VideoSourceSubscription::VideoSourceSubscription(size_t queue_depth)
    : queue_depth_(queue_depth == 0 ? 1 : queue_depth) {}

VideoSourceSubscription::~VideoSourceSubscription() {
  Close();
}

bool VideoSourceSubscription::WaitNext(VideoFramePtr* frame,
                                       std::chrono::milliseconds timeout) {
  if (!frame) {
    return false;
  }

  std::unique_lock<std::mutex> lock(mutex_);
  condition_.wait_for(lock, timeout,
                      [this] { return closed_ || !queue_.empty(); });
  if (queue_.empty()) {
    return false;
  }

  *frame = queue_.front();
  queue_.pop_front();
  return true;
}

void VideoSourceSubscription::Close() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    closed_ = true;
    queue_.clear();
  }
  condition_.notify_all();
}

void VideoSourceSubscription::Push(VideoFramePtr frame) {
  if (!frame) {
    return;
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_) {
      return;
    }
    queue_.push_back(frame);
    while (queue_.size() > queue_depth_) {
      queue_.pop_front();
    }
  }
  condition_.notify_one();
}

AsyncDualCameraVideoSource::AsyncDualCameraVideoSource(
    const DualCameraVideoSourceConfig& config)
    : config_(config) {}

AsyncDualCameraVideoSource::~AsyncDualCameraVideoSource() {
  Stop();
}

std::shared_ptr<VideoSourceSubscription> AsyncDualCameraVideoSource::Subscribe(
    size_t queue_depth) {
  std::shared_ptr<VideoSourceSubscription> subscription(
      new VideoSourceSubscription(queue_depth));

  bool close_immediately = false;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    close_immediately = failed_;
    subscriptions_.push_back(subscription);
  }

  if (close_immediately) {
    subscription->Close();
  }

  return subscription;
}

void AsyncDualCameraVideoSource::Start() {
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (started_) {
      return;
    }
  }

  if (capture_thread_.joinable()) {
    capture_thread_.join();
  }

  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    stop_requested_ = false;
    failed_ = false;
    error_message_.clear();
    captured_frames_ = 0;
    started_ = true;
  }

  capture_thread_ = std::thread(&AsyncDualCameraVideoSource::CaptureLoop, this);
}

void AsyncDualCameraVideoSource::Stop() {
  bool should_close_only = false;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!started_ && !capture_thread_.joinable()) {
      should_close_only = true;
    } else {
      stop_requested_ = true;
    }
  }

  if (should_close_only) {
    CloseSubscriptions();
    return;
  }

  if (capture_thread_.joinable()) {
    capture_thread_.join();
  }

  CloseSubscriptions();
}

bool AsyncDualCameraVideoSource::running() const {
  std::lock_guard<std::mutex> lock(state_mutex_);
  return started_ && !stop_requested_ && !failed_;
}

bool AsyncDualCameraVideoSource::failed() const {
  std::lock_guard<std::mutex> lock(state_mutex_);
  return failed_;
}

std::string AsyncDualCameraVideoSource::error_message() const {
  std::lock_guard<std::mutex> lock(state_mutex_);
  return error_message_;
}

uint64_t AsyncDualCameraVideoSource::captured_frames() const {
  std::lock_guard<std::mutex> lock(state_mutex_);
  return captured_frames_;
}

void AsyncDualCameraVideoSource::CaptureLoop() {
  try {
    rtc_camera::CameraCaptureOptions left_camera_options = config_.options;
    left_camera_options.device = config_.left_device;

    rtc_camera::V4l2CameraDevice left_device;
    left_device.Open(left_camera_options);
    LogCameraInfo("left camera", left_device);

    rtc_camera::CameraCaptureOptions right_camera_options = config_.options;
    right_camera_options.device = config_.right_device;
    if (right_camera_options.width == 0) {
      right_camera_options.width = static_cast<int>(left_device.width());
    }
    if (right_camera_options.height == 0) {
      right_camera_options.height = static_cast<int>(left_device.height());
    }

    rtc_camera::V4l2CameraDevice right_device;
    right_device.Open(right_camera_options);
    LogCameraInfo("right camera", right_device);

    if (left_device.height() != right_device.height()) {
      throw std::runtime_error(
          "camera heights do not match, unable to stitch side-by-side");
    }

    rtc_logging::LogInfo(std::string("sampled left output: ") +
            std::to_string(left_device.width() / 2) + "x" +
            std::to_string(left_device.height()));
    rtc_logging::LogInfo(std::string("sampled right output: ") +
            std::to_string(right_device.width() / 2) + "x" +
            std::to_string(right_device.height()));
    rtc_logging::LogInfo(std::string("stitched output: ") +
            std::to_string(left_device.width() / 2 + right_device.width() / 2) +
            "x" + std::to_string(left_device.height()));

    RunDualWarmup(left_device, right_device, config_.options, this);

    bool first_frame_logged = false;
    uint64_t sequence = 0;
    while (!StopRequestedBySource(this)) {
      rtc_camera::V4l2CameraDevice::CapturedFrame left_frame;
      if (!left_device.DequeueCapturedFrame(&left_frame)) {
        continue;
      }
      CapturedFrameGuard left_guard(&left_device, &left_frame);

      rtc_camera::V4l2CameraDevice::CapturedFrame right_frame;
      if (!right_device.DequeueCapturedFrame(&right_frame)) {
        continue;
      }
      CapturedFrameGuard right_guard(&right_device, &right_frame);

      std::shared_ptr<VideoFrame> frame(new VideoFrame());
      frame->format = VideoFrameFormat::kDualUyvy;
      frame->sequence = ++sequence;
      frame->timestamp_us = NowMicros();
      frame->left_pixel_format = left_device.pixel_format();
      frame->right_pixel_format = right_device.pixel_format();
      frame->width = left_device.width() / 2 + right_device.width() / 2;
      frame->height = left_device.height();
      frame->left_width = left_device.width();
      frame->left_height = left_device.height();
      frame->left_stride_bytes = left_device.bytes_per_line();
      frame->left_data.resize(left_frame.bytes_used);
      std::memcpy(frame->left_data.data(), left_frame.data,
                  left_frame.bytes_used);
      frame->right_width = right_device.width();
      frame->right_height = right_device.height();
      frame->right_stride_bytes = right_device.bytes_per_line();
      frame->right_data.resize(right_frame.bytes_used);
      std::memcpy(frame->right_data.data(), right_frame.data,
                  right_frame.bytes_used);

      {
        std::lock_guard<std::mutex> lock(state_mutex_);
        ++captured_frames_;
      }

      if (!first_frame_logged) {
        first_frame_logged = true;
        rtc_logging::LogInfo("first dual UYVY frame pair captured");
      }

      Publish(frame);
    }
  } catch (const std::exception& ex) {
    SetError(ex.what());
  }

  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    started_ = false;
    stop_requested_ = true;
  }
  CloseSubscriptions();
}

void AsyncDualCameraVideoSource::Publish(VideoFramePtr frame) {
  std::vector<std::shared_ptr<VideoSourceSubscription>> subscriptions;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    subscriptions_.erase(
        std::remove_if(subscriptions_.begin(), subscriptions_.end(),
                       [](const std::weak_ptr<VideoSourceSubscription>& weak) {
                         return weak.expired();
                       }),
        subscriptions_.end());
    for (const auto& weak : subscriptions_) {
      std::shared_ptr<VideoSourceSubscription> subscription = weak.lock();
      if (subscription) {
        subscriptions.push_back(subscription);
      }
    }
  }

  for (const auto& subscription : subscriptions) {
    subscription->Push(frame);
  }
}

void AsyncDualCameraVideoSource::CloseSubscriptions() {
  std::vector<std::shared_ptr<VideoSourceSubscription>> subscriptions;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    for (const auto& weak : subscriptions_) {
      std::shared_ptr<VideoSourceSubscription> subscription = weak.lock();
      if (subscription) {
        subscriptions.push_back(subscription);
      }
    }
    subscriptions_.clear();
  }

  for (const auto& subscription : subscriptions) {
    subscription->Close();
  }
}

void AsyncDualCameraVideoSource::SetError(const std::string& error_message) {
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    failed_ = true;
    error_message_ = error_message;
  }
  rtc_logging::LogError(std::string("dual camera video source failed: ") + error_message);
}

}  // 命名空间 internal
}  // 命名空间 rtc_dual_camera
