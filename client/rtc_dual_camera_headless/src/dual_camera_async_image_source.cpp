#include "rtc_dual_camera/dual_camera_async_image_source.h"

#include "async_dual_camera_video_source.h"

namespace rtc_dual_camera {
namespace {

rtc_camera_headless::DualCameraVideoSourceConfig MakeVideoSourceConfig(
    const AsyncDualCameraImageSourceOptions& options) {
  rtc_camera_headless::DualCameraVideoSourceConfig config;
  config.left_device = options.left_device;
  config.right_device = options.right_device;
  config.options.width = options.width;
  config.options.height = options.height;
  config.options.buffer_count = options.buffer_count;
  config.options.timeout_ms = options.timeout_ms;
  config.options.warmup_frames = options.warmup_frames;
  config.options.warmup_delay_ms = options.warmup_delay_ms;
  return config;
}

ImagePixelFormat ConvertFormat(
    rtc_camera_headless::VideoFrameFormat source_format) {
  switch (source_format) {
    case rtc_camera_headless::VideoFrameFormat::kI420:
      return ImagePixelFormat::kI420;
    case rtc_camera_headless::VideoFrameFormat::kDualUyvy:
      return ImagePixelFormat::kDualUyvy;
  }
  return ImagePixelFormat::kI420;
}

void CopyFrame(const rtc_camera_headless::VideoFrame& source,
               ImageFrame* destination) {
  destination->format = ConvertFormat(source.format);
  destination->sequence = source.sequence;
  destination->timestamp_us = source.timestamp_us;
  destination->width = source.width;
  destination->height = source.height;
  destination->stride_y = source.stride_y;
  destination->stride_u = source.stride_u;
  destination->stride_v = source.stride_v;
  destination->data = source.data.empty() ? nullptr : source.data.data();
  destination->data_size = source.data.size();
  destination->left_pixel_format = source.left_pixel_format;
  destination->right_pixel_format = source.right_pixel_format;
  destination->left_width = source.left_width;
  destination->left_height = source.left_height;
  destination->left_stride_bytes = source.left_stride_bytes;
  destination->left_data =
      source.left_data.empty() ? nullptr : source.left_data.data();
  destination->left_data_size = source.left_data.size();
  destination->right_width = source.right_width;
  destination->right_height = source.right_height;
  destination->right_stride_bytes = source.right_stride_bytes;
  destination->right_data =
      source.right_data.empty() ? nullptr : source.right_data.data();
  destination->right_data_size = source.right_data.size();
}

}  // namespace

bool ImageFrame::empty() const {
  if (format == ImagePixelFormat::kDualUyvy) {
    return !left_data || left_data_size == 0 || !right_data ||
           right_data_size == 0;
  }
  return !data || data_size == 0;
}

struct AsyncImageFrameSubscription::Impl {
  explicit Impl(
      const std::shared_ptr<rtc_camera_headless::VideoSourceSubscription>&
          source_subscription)
      : source_subscription(source_subscription) {}

  std::shared_ptr<rtc_camera_headless::VideoSourceSubscription>
      source_subscription;
};

AsyncImageFrameSubscription::AsyncImageFrameSubscription(
    const std::shared_ptr<Impl>& impl)
    : impl_(impl) {}

AsyncImageFrameSubscription::~AsyncImageFrameSubscription() {
  Close();
}

bool AsyncImageFrameSubscription::WaitNext(
    ImageFrame* frame,
    std::chrono::milliseconds timeout) {
  if (!frame || !impl_ || !impl_->source_subscription) {
    return false;
  }

  rtc_camera_headless::VideoFramePtr source_frame;
  if (!impl_->source_subscription->WaitNext(&source_frame, timeout) ||
      !source_frame) {
    return false;
  }

  CopyFrame(*source_frame, frame);
  frame->owner_ = source_frame;
  return true;
}

void AsyncImageFrameSubscription::Close() {
  if (impl_ && impl_->source_subscription) {
    impl_->source_subscription->Close();
  }
}

struct AsyncDualCameraImageSource::Impl {
  explicit Impl(const AsyncDualCameraImageSourceOptions& options)
      : source(MakeVideoSourceConfig(options)) {}

  rtc_camera_headless::AsyncDualCameraVideoSource source;
};

AsyncDualCameraImageSource::AsyncDualCameraImageSource(
    const AsyncDualCameraImageSourceOptions& options)
    : impl_(new Impl(options)) {}

AsyncDualCameraImageSource::~AsyncDualCameraImageSource() {
  Stop();
}

std::shared_ptr<AsyncImageFrameSubscription>
AsyncDualCameraImageSource::Subscribe(size_t queue_depth) {
  if (!impl_) {
    return std::shared_ptr<AsyncImageFrameSubscription>();
  }

  std::shared_ptr<AsyncImageFrameSubscription::Impl> subscription_impl(
      new AsyncImageFrameSubscription::Impl(
          impl_->source.Subscribe(queue_depth)));
  return std::shared_ptr<AsyncImageFrameSubscription>(
      new AsyncImageFrameSubscription(subscription_impl));
}

void AsyncDualCameraImageSource::Start() {
  if (impl_) {
    impl_->source.Start();
  }
}

void AsyncDualCameraImageSource::Stop() {
  if (impl_) {
    impl_->source.Stop();
  }
}

bool AsyncDualCameraImageSource::running() const {
  return impl_ && impl_->source.running();
}

bool AsyncDualCameraImageSource::failed() const {
  return impl_ && impl_->source.failed();
}

std::string AsyncDualCameraImageSource::error_message() const {
  return impl_ ? impl_->source.error_message() : std::string();
}

uint64_t AsyncDualCameraImageSource::captured_frames() const {
  return impl_ ? impl_->source.captured_frames() : 0;
}

}  // namespace rtc_dual_camera
