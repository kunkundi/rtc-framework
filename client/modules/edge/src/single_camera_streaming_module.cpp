#include "rtc_edge/single_camera_streaming_module.h"

#include "rtc_camera/camera_frame_converter.h"
#include "rtc_logging/rtc_logging.h"
#include "rtc_runtime/rtc_session.h"

#include <stdexcept>

namespace rtc_edge {

SingleCameraStreamingModule::SingleCameraStreamingModule(
    const SingleCameraStreamingModuleOptions& options)
    : options_(options) {}

SingleCameraStreamingModule::~SingleCameraStreamingModule() {
  Stop();
}

bool SingleCameraStreamingModule::Start(std::string* error_message) {
  if (started_) {
    return true;
  }
  if (options_.capture.device.empty()) {
    if (error_message != nullptr) {
      *error_message = "Camera device must not be empty";
    }
    return false;
  }
  if (options_.video_source_id.empty()) {
    if (error_message != nullptr) {
      *error_message = "RTC video source ID must not be empty";
    }
    return false;
  }

  try {
    video_source_.reset(
        new rtc_camera::AsyncCameraImageSource(options_.capture));
    rtc_frames_ = video_source_->Subscribe(1);
    if (!rtc_frames_) {
      if (error_message != nullptr) {
        *error_message = "Failed to create camera frame subscription";
      }
      video_source_.reset();
      return false;
    }

    converter_.reset(new rtc_camera::CameraFrameConverter());
    video_source_->Start();
    started_ = true;
    first_frame_logged_ = false;
    return true;
  } catch (const std::exception& ex) {
    if (error_message != nullptr) {
      *error_message = ex.what();
    }
    Stop();
    return false;
  }
}

void SingleCameraStreamingModule::Stop() {
  RequestStop();
  if (video_source_) {
    video_source_->Stop();
  }
  converter_.reset();
  rtc_frames_.reset();
  video_source_.reset();
  started_ = false;
}

void SingleCameraStreamingModule::RequestStop() {
  if (rtc_frames_) {
    rtc_frames_->Close();
  }
  if (video_source_) {
    video_source_->RequestStop();
  }
}

bool SingleCameraStreamingModule::Tick(
    rtc_runtime::RtcSession* rtc_session,
    std::string* error_message) {
  if (!started_ || rtc_session == nullptr || !video_source_ || !rtc_frames_ ||
      !converter_) {
    if (error_message != nullptr) {
      *error_message = "Single camera module has not been started";
    }
    return false;
  }

  rtc_camera::CameraFrame frame;
  if (!rtc_frames_->WaitNext(&frame, options_.frame_wait)) {
    if (video_source_->failed()) {
      if (error_message != nullptr) {
        *error_message = options_.camera_name + " capture failed: " +
                         video_source_->error_message();
      }
      return false;
    }
    return true;
  }

  rtc_session->NoteCapturedFrame();
  if (!first_frame_logged_) {
    first_frame_logged_ = true;
    rtc_logging::LogInfo(options_.camera_name +
                         " received its first camera frame");
  }
  if (!rtc_session->IsReadyToSend()) {
    return true;
  }
  if (frame.empty()) {
    if (error_message != nullptr) {
      *error_message = options_.camera_name + " received an invalid frame";
    }
    return false;
  }

  rtc_camera::ConvertedCameraFrame converted;
  std::string convert_error;
  if (!converter_->ConvertToI420(frame, &converted, &convert_error)) {
    if (error_message != nullptr) {
      *error_message = options_.camera_name + " conversion failed: " +
                       convert_error;
    }
    return false;
  }

  if (!rtc_session->SendI420Frame(
          options_.video_source_id.c_str(), converted.data,
          converted.data_size, converted.width, converted.height,
          converted.stride_y, converted.stride_u, converted.stride_v)) {
    if (error_message != nullptr) {
      *error_message = options_.camera_name + " RTC frame send failed";
    }
    return false;
  }
  return true;
}

bool SingleCameraStreamingModule::started() const {
  return started_;
}

uint64_t SingleCameraStreamingModule::captured_frames() const {
  return video_source_ ? video_source_->captured_frames() : 0;
}

}  // 命名空间 rtc_edge
