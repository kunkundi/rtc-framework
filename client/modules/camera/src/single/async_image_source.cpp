#include "rtc_camera/single/async_image_source.h"

#include "rtc_camera/v4l2_camera.h"
#include "rtc_logging/rtc_logging.h"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

namespace rtc_camera {
namespace single {
namespace {

struct OwnedCameraFrame {
  ~OwnedCameraFrame() {
    if (device && captured_frame.data) {
      try {
        device->RequeueCapturedFrame(&captured_frame);
      } catch (const std::exception& ex) {
        rtc_logging::LogError(std::string("camera buffer requeue failed: ") +
                              ex.what());
      }
    }
  }

  uint64_t sequence = 0;
  int64_t timestamp_us = 0;
  uint32_t pixel_format = 0;
  size_t width = 0;
  size_t height = 0;
  size_t stride_bytes = 0;
  const uint8_t* data = nullptr;
  size_t data_size = 0;
  std::shared_ptr<V4l2CameraDevice> device;
  V4l2CameraDevice::CapturedFrame captured_frame;
};

int64_t NowMicros() {
  const std::chrono::steady_clock::time_point now =
      std::chrono::steady_clock::now();
  return std::chrono::duration_cast<std::chrono::microseconds>(
             now.time_since_epoch())
      .count();
}

class CapturedFrameGuard {
 public:
  CapturedFrameGuard(V4l2CameraDevice* device,
                     V4l2CameraDevice::CapturedFrame* frame)
      : device_(device), frame_(frame) {}

  ~CapturedFrameGuard() {
    if (device_ != nullptr && frame_ != nullptr && frame_->data != nullptr) {
      device_->RequeueCapturedFrame(frame_);
    }
  }

  CapturedFrameGuard(const CapturedFrameGuard&) = delete;
  CapturedFrameGuard& operator=(const CapturedFrameGuard&) = delete;

  void Release() {
    device_ = nullptr;
    frame_ = nullptr;
  }

 private:
  V4l2CameraDevice* device_ = nullptr;
  V4l2CameraDevice::CapturedFrame* frame_ = nullptr;
};

}  // 匿名命名空间

struct CameraFrameSubscription::Impl {
  explicit Impl(size_t requested_queue_depth)
      : queue_depth(requested_queue_depth == 0 ? 1 : requested_queue_depth) {}

  void Push(const std::shared_ptr<const OwnedCameraFrame>& frame) {
    if (!frame) {
      return;
    }

    {
      std::lock_guard<std::mutex> lock(mutex);
      if (closed) {
        return;
      }
      queue.push_back(frame);
      // 实时视频优先保留最新帧，队列满时丢弃最旧帧。
      while (queue.size() > queue_depth) {
        queue.pop_front();
      }
    }
    condition.notify_one();
  }

  void Close() {
    {
      std::lock_guard<std::mutex> lock(mutex);
      closed = true;
      queue.clear();
    }
    condition.notify_all();
  }

  size_t queue_depth = 1;
  bool closed = false;
  std::mutex mutex;
  std::condition_variable condition;
  std::deque<std::shared_ptr<const OwnedCameraFrame>> queue;
};

bool CameraFrame::empty() const {
  return data == nullptr || data_size == 0;
}

CameraFrameSubscription::CameraFrameSubscription(
    const std::shared_ptr<Impl>& impl)
    : impl_(impl) {}

CameraFrameSubscription::~CameraFrameSubscription() {
  Close();
}

bool CameraFrameSubscription::WaitNext(
    CameraFrame* frame,
    std::chrono::milliseconds timeout) {
  if (frame == nullptr || !impl_) {
    return false;
  }
  *frame = CameraFrame();

  std::shared_ptr<const OwnedCameraFrame> source_frame;
  {
    std::unique_lock<std::mutex> lock(impl_->mutex);
    impl_->condition.wait_for(lock, timeout, [this] {
      return impl_->closed || !impl_->queue.empty();
    });
    if (impl_->queue.empty()) {
      return false;
    }
    source_frame = impl_->queue.front();
    impl_->queue.pop_front();
  }

  frame->sequence = source_frame->sequence;
  frame->timestamp_us = source_frame->timestamp_us;
  frame->pixel_format = source_frame->pixel_format;
  frame->width = source_frame->width;
  frame->height = source_frame->height;
  frame->stride_bytes = source_frame->stride_bytes;
  frame->data = source_frame->data;
  frame->data_size = source_frame->data_size;
  frame->owner_ = source_frame;
  frame->device_owner_ = source_frame->device;
  frame->mmap_backed_ = true;
  return true;
}

void CameraFrameSubscription::Close() {
  if (impl_) {
    impl_->Close();
  }
}

struct AsyncCameraImageSource::Impl {
  explicit Impl(const CameraCaptureOptions& source_options)
      : options(source_options) {}

  ~Impl() {
    Stop();
  }

  std::shared_ptr<CameraFrameSubscription> Subscribe(size_t queue_depth) {
    std::shared_ptr<CameraFrameSubscription::Impl> subscription_impl(
        new CameraFrameSubscription::Impl(queue_depth));

    bool close_immediately = false;
    {
      std::lock_guard<std::mutex> lock(state_mutex);
      close_immediately = failed;
      subscriptions.erase(
          std::remove_if(
              subscriptions.begin(), subscriptions.end(),
              [](const std::weak_ptr<CameraFrameSubscription::Impl>& weak) {
                return weak.expired();
              }),
          subscriptions.end());
      subscriptions.push_back(subscription_impl);
    }
    if (close_immediately) {
      subscription_impl->Close();
    }

    return std::shared_ptr<CameraFrameSubscription>(
        new CameraFrameSubscription(subscription_impl));
  }

  void Start() {
    {
      std::lock_guard<std::mutex> lock(state_mutex);
      if (started) {
        return;
      }
    }

    if (capture_thread.joinable()) {
      capture_thread.join();
    }

    {
      std::lock_guard<std::mutex> lock(state_mutex);
      stop_requested = false;
      failed = false;
      error_message.clear();
      captured_frames = 0;
      started = true;
    }
    capture_thread = std::thread(&Impl::CaptureLoop, this);
  }

  void Stop() {
    // 先发停止请求再等待线程退出，调用方可以并行通知多路相机。
    RequestStop();
    if (capture_thread.joinable()) {
      capture_thread.join();
    }
    CloseSubscriptions();
  }

  void RequestStop() {
    {
      std::lock_guard<std::mutex> lock(state_mutex);
      stop_requested = true;
    }
  }

  bool IsRunning() const {
    std::lock_guard<std::mutex> lock(state_mutex);
    return started && !stop_requested && !failed;
  }

  void CaptureLoop() {
    try {
      std::shared_ptr<V4l2CameraDevice> device(new V4l2CameraDevice());
      device->Open(options);
      rtc_logging::LogInfo(
          std::string("camera source: ") + device->device_path() + " " +
          std::to_string(device->width()) + "x" +
          std::to_string(device->height()) + " " +
          PixelFormatToString(device->pixel_format()));

      RunWarmup(*device);

      uint64_t sequence = 0;
      while (IsRunning()) {
        V4l2CameraDevice::CapturedFrame captured_frame;
        if (!device->DequeueCapturedFrame(&captured_frame)) {
          continue;
        }
        CapturedFrameGuard frame_guard(device.get(), &captured_frame);

        std::shared_ptr<OwnedCameraFrame> frame(new OwnedCameraFrame());
        frame->sequence = ++sequence;
        frame->timestamp_us = NowMicros();
        frame->pixel_format = device->pixel_format();
        frame->width = device->width();
        frame->height = device->height();
        frame->stride_bytes = device->bytes_per_line();
        frame->data = captured_frame.data;
        frame->data_size = captured_frame.bytes_used;
        frame->device = device;
        frame->captured_frame = captured_frame;
        frame_guard.Release();

        {
          std::lock_guard<std::mutex> lock(state_mutex);
          ++captured_frames;
        }
        Publish(frame);
      }
    } catch (const std::exception& ex) {
      SetError(ex.what());
    }

    {
      std::lock_guard<std::mutex> lock(state_mutex);
      started = false;
      stop_requested = true;
    }
    CloseSubscriptions();
  }

  void RunWarmup(V4l2CameraDevice& device) {
    if (options.warmup_delay_ms > 0) {
      std::this_thread::sleep_for(
          std::chrono::milliseconds(options.warmup_delay_ms));
    }

    int discarded = 0;
    while (IsRunning() && discarded < options.warmup_frames) {
      V4l2CameraDevice::CapturedFrame frame;
      if (!device.DequeueCapturedFrame(&frame)) {
        continue;
      }
      CapturedFrameGuard frame_guard(&device, &frame);
      ++discarded;
    }
  }

  void Publish(const std::shared_ptr<const OwnedCameraFrame>& frame) {
    std::vector<std::weak_ptr<CameraFrameSubscription::Impl>> snapshot;
    {
      std::lock_guard<std::mutex> lock(state_mutex);
      snapshot = subscriptions;
    }

    for (size_t i = 0; i < snapshot.size(); ++i) {
      std::shared_ptr<CameraFrameSubscription::Impl> subscription =
          snapshot[i].lock();
      if (subscription) {
        subscription->Push(frame);
      }
    }
  }

  void CloseSubscriptions() {
    std::vector<std::shared_ptr<CameraFrameSubscription::Impl>> active;
    {
      std::lock_guard<std::mutex> lock(state_mutex);
      for (size_t i = 0; i < subscriptions.size(); ++i) {
        std::shared_ptr<CameraFrameSubscription::Impl> subscription =
            subscriptions[i].lock();
        if (subscription) {
          active.push_back(subscription);
        }
      }
      subscriptions.clear();
    }

    for (size_t i = 0; i < active.size(); ++i) {
      active[i]->Close();
    }
  }

  void SetError(const std::string& message) {
    {
      std::lock_guard<std::mutex> lock(state_mutex);
      failed = true;
      error_message = message;
    }
    rtc_logging::LogError(std::string("camera source failed: ") + message);
  }

  CameraCaptureOptions options;
  mutable std::mutex state_mutex;
  bool started = false;
  bool stop_requested = false;
  bool failed = false;
  std::string error_message;
  uint64_t captured_frames = 0;
  std::thread capture_thread;
  std::vector<std::weak_ptr<CameraFrameSubscription::Impl>> subscriptions;
};

AsyncCameraImageSource::AsyncCameraImageSource(
    const CameraCaptureOptions& options)
    : impl_(new Impl(options)) {}

AsyncCameraImageSource::~AsyncCameraImageSource() {
  Stop();
}

std::shared_ptr<CameraFrameSubscription> AsyncCameraImageSource::Subscribe(
    size_t queue_depth) {
  if (!impl_) {
    return std::shared_ptr<CameraFrameSubscription>();
  }
  return impl_->Subscribe(queue_depth);
}

void AsyncCameraImageSource::Start() {
  if (impl_) {
    impl_->Start();
  }
}

void AsyncCameraImageSource::Stop() {
  if (impl_) {
    impl_->Stop();
  }
}

void AsyncCameraImageSource::RequestStop() {
  if (impl_) {
    impl_->RequestStop();
  }
}

bool AsyncCameraImageSource::running() const {
  return impl_ && impl_->IsRunning();
}

bool AsyncCameraImageSource::failed() const {
  if (!impl_) {
    return false;
  }
  std::lock_guard<std::mutex> lock(impl_->state_mutex);
  return impl_->failed;
}

std::string AsyncCameraImageSource::error_message() const {
  if (!impl_) {
    return std::string();
  }
  std::lock_guard<std::mutex> lock(impl_->state_mutex);
  return impl_->error_message;
}

uint64_t AsyncCameraImageSource::captured_frames() const {
  if (!impl_) {
    return 0;
  }
  std::lock_guard<std::mutex> lock(impl_->state_mutex);
  return impl_->captured_frames;
}

}  // 命名空间 single
}  // 命名空间 rtc_camera
