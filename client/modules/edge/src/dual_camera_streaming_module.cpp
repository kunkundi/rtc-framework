#include "rtc_edge/dual_camera_streaming_module.h"

#include "rtc_edge/camera_video_sources.h"

#include "rtc_camera/dual/frame_converter.h"
#include "rtc_logging/rtc_logging.h"
#include "rtc_runtime/rtc_session.h"
#include "rtc_vision/yolo_frame_consumer.h"

#include <exception>

namespace rtc_edge {

DualCameraStreamingModule::DualCameraStreamingModule(
    const DualCameraStreamingModuleOptions& options)
    : options_(options) {}

DualCameraStreamingModule::~DualCameraStreamingModule() {
  Stop();
}

bool DualCameraStreamingModule::Start(std::string* error_message) {
  if (started_) {
    return true;
  }
#ifndef VTSRTC_ENABLE_YOLO_TENSORRT
  if (options_.yolo_enabled) {
    if (error_message != nullptr) {
      *error_message =
          "YOLO is enabled in rtc.cfg, but this binary was built without "
          "--enable_yolo=y";
    }
    return false;
  }
#endif
  try {
    video_source_.reset(
        new rtc_camera::dual::AsyncDualCameraImageSource(options_.capture));
    rtc_frames_ = video_source_->Subscribe(2);
    if (!rtc_frames_) {
      if (error_message) {
        *error_message = "failed to create RTC camera frame subscription";
      }
      video_source_.reset();
      return false;
    }
<<<<<<< HEAD
    if (options_.yolo_enabled) {
      yolo_frames_ = video_source_->Subscribe(1);
      if (!yolo_frames_) {
        if (error_message != nullptr) {
          *error_message = "failed to create YOLO camera frame subscription";
        }
        Stop();
        return false;
      }
    }
    converter_.reset(new rtc_camera::dual::DualUyvyFrameConverter());
    video_source_->Start();
    if (options_.yolo_enabled) {
      rtc_camera_headless::YoloFrameConsumerOptions yolo_options;
      yolo_options.max_fps = options_.yolo_max_fps;
      yolo_options.processing_downscale =
          options_.yolo_processing_downscale;
      yolo_consumer_.reset(new rtc_camera_headless::YoloFrameConsumer(
          yolo_frames_, yolo_options));
      yolo_consumer_->Start();
=======
    converter_.reset(new rtc_camera::dual::DualUyvyFrameConverter());
    video_source_->Start();
    if (!StartYoloConsumer(error_message)) {
      Stop();
      return false;
>>>>>>> 5c8f59e (新加双目和环路切换)
    }
    started_ = true;
    first_frame_logged_ = false;
    return true;
  } catch (const std::exception& ex) {
    if (error_message) {
      *error_message = ex.what();
    }
    Stop();
    return false;
  }
}

void DualCameraStreamingModule::Stop() {
<<<<<<< HEAD
  if (yolo_consumer_) {
    yolo_consumer_->Stop();
  }
=======
  StopYoloConsumer();
>>>>>>> 5c8f59e (新加双目和环路切换)
  if (rtc_frames_) {
    rtc_frames_->Close();
  }
  if (video_source_) {
    video_source_->Stop();
  }
  converter_.reset();
<<<<<<< HEAD
  yolo_consumer_.reset();
  yolo_frames_.reset();
=======
>>>>>>> 5c8f59e (新加双目和环路切换)
  rtc_frames_.reset();
  video_source_.reset();
  started_ = false;
}

bool DualCameraStreamingModule::Tick(
    rtc_runtime::RtcSession* rtc_session,
<<<<<<< HEAD
    std::string* error_message) {
=======
    std::string* error_message,
    bool send_frame,
    bool run_yolo) {
>>>>>>> 5c8f59e (新加双目和环路切换)
  if (!started_ || !rtc_session || !video_source_ || !rtc_frames_ ||
      !converter_) {
    if (error_message) {
      *error_message = "dual camera streaming module is not started";
    }
    return false;
  }

<<<<<<< HEAD
  rtc_camera::dual::ImageFrame frame;
  if (!rtc_frames_->WaitNext(&frame, options_.frame_wait)) {
=======
  if (run_yolo) {
    if (!StartYoloConsumer(error_message)) {
      return false;
    }
  } else {
    StopYoloConsumer();
  }

  rtc_camera::dual::ImageFrame frame;
  if (!DrainLatestFrame(&frame, options_.frame_wait)) {
>>>>>>> 5c8f59e (新加双目和环路切换)
    if (video_source_->failed()) {
      if (error_message) {
        *error_message =
            std::string("camera capture failed: ") + video_source_->error_message();
      }
      return false;
    }
    return true;
  }

  rtc_session->NoteCapturedFrame();
  if (!first_frame_logged_) {
    first_frame_logged_ = true;
    rtc_logging::LogInfo("Camera module received first raw stereo frame");
  }
  if (!rtc_session->IsReadyToSend()) {
    return true;
  }
  if (frame.empty()) {
    if (error_message) {
      *error_message = "camera module received an invalid frame";
    }
    return false;
  }
<<<<<<< HEAD
=======
  if (!send_frame) {
    return true;
  }
>>>>>>> 5c8f59e (新加双目和环路切换)

  rtc_camera::dual::ConvertedI420Frame converted;
  std::string convert_error;
  if (!converter_->ConvertToI420(frame, &converted, &convert_error)) {
    if (error_message) {
      *error_message = std::string("camera pixel format conversion failed: ") +
                       convert_error;
    }
    return false;
  }
  if (!rtc_session->SendI420Frame(
          kStereoCameraVideoSourceId, converted.data, converted.data_size,
          converted.width,
          converted.height, converted.stride_y, converted.stride_u,
          converted.stride_v)) {
    if (error_message) {
      *error_message = "failed to send RTC video frame";
    }
    return false;
  }
  return true;
}

<<<<<<< HEAD
=======
bool DualCameraStreamingModule::StartYoloConsumer(
    std::string* error_message) {
  if (!options_.yolo_enabled || yolo_active_) {
    return true;
  }
  if (!video_source_) {
    if (error_message != nullptr) {
      *error_message = "failed to start YOLO: camera source is not ready";
    }
    return false;
  }

  yolo_frames_ = video_source_->Subscribe(1);
  if (!yolo_frames_) {
    if (error_message != nullptr) {
      *error_message = "failed to create YOLO camera frame subscription";
    }
    return false;
  }

  rtc_camera_headless::YoloFrameConsumerOptions yolo_options;
  yolo_options.max_fps = options_.yolo_max_fps;
  yolo_options.processing_downscale = options_.yolo_processing_downscale;
  yolo_consumer_.reset(new rtc_camera_headless::YoloFrameConsumer(
      yolo_frames_, yolo_options));
  yolo_consumer_->Start();
  yolo_active_ = true;
  return true;
}

void DualCameraStreamingModule::StopYoloConsumer() {
  if (yolo_consumer_) {
    yolo_consumer_->Stop();
  } else if (yolo_frames_) {
    yolo_frames_->Close();
  }
  yolo_consumer_.reset();
  yolo_frames_.reset();
  yolo_active_ = false;
}

bool DualCameraStreamingModule::DrainLatestFrame(
    rtc_camera::dual::ImageFrame* frame,
    std::chrono::milliseconds first_wait) const {
  if (!frame || !rtc_frames_->WaitNext(frame, first_wait)) {
    return false;
  }

  rtc_camera::dual::ImageFrame latest;
  while (rtc_frames_->WaitNext(&latest, std::chrono::milliseconds(0))) {
    *frame = latest;
  }
  return true;
}

>>>>>>> 5c8f59e (新加双目和环路切换)
uint64_t DualCameraStreamingModule::captured_frames() const {
  return video_source_ ? video_source_->captured_frames() : 0;
}

}  // 命名空间 rtc_edge
