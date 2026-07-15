#include "rtc_edge/dual_camera_streaming_module.h"

#include "rtc_edge/camera_video_sources.h"

#include "rtc_dual_camera/dual_uyvy_frame_converter.h"
#include "rtc_headless/rtc_camera_common.h"
#include "rtc_headless/rtc_headless_session.h"
#include "rtc_logging/rtc_logging.h"
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
        new rtc_dual_camera::AsyncDualCameraImageSource(options_.capture));
    rtc_frames_ = video_source_->Subscribe(2);
    if (!rtc_frames_) {
      if (error_message) {
        *error_message = "创建摄像头帧订阅失败";
      }
      video_source_.reset();
      return false;
    }
    if (options_.yolo_enabled) {
      yolo_frames_ = video_source_->Subscribe(1);
      if (!yolo_frames_) {
        if (error_message != nullptr) {
          *error_message = "创建 YOLO 帧订阅失败";
        }
        Stop();
        return false;
      }
    }
    converter_.reset(new rtc_camera_headless::DualUyvyFrameConverter());
    video_source_->Start();
    if (options_.yolo_enabled) {
      rtc_camera_headless::YoloFrameConsumerOptions yolo_options;
      yolo_options.processing_downscale =
          options_.yolo_processing_downscale;
      yolo_consumer_.reset(new rtc_camera_headless::YoloFrameConsumer(
          yolo_frames_, yolo_options));
      yolo_consumer_->Start();
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
  if (yolo_consumer_) {
    yolo_consumer_->Stop();
  }
  if (rtc_frames_) {
    rtc_frames_->Close();
  }
  if (video_source_) {
    video_source_->Stop();
  }
  converter_.reset();
  yolo_consumer_.reset();
  yolo_frames_.reset();
  rtc_frames_.reset();
  video_source_.reset();
  started_ = false;
}

bool DualCameraStreamingModule::Tick(
    rtc_camera_headless::RtcHeadlessSession* rtc_session,
    std::string* error_message) {
  if (!started_ || !rtc_session || !video_source_ || !rtc_frames_ ||
      !converter_) {
    if (error_message) {
      *error_message = "摄像头模块尚未启动";
    }
    return false;
  }

  rtc_dual_camera::ImageFrame frame;
  if (!rtc_frames_->WaitNext(&frame, options_.frame_wait)) {
    if (video_source_->failed()) {
      if (error_message) {
        *error_message =
            std::string("摄像头采集失败：") + video_source_->error_message();
      }
      return false;
    }
    return true;
  }

  rtc_session->NoteCapturedFrame();
  if (!first_frame_logged_) {
    first_frame_logged_ = true;
    rtc_logging::LogInfo("摄像头模块收到首个双目原始帧");
  }
  if (!rtc_session->IsReadyToSend()) {
    return true;
  }
  if (frame.empty()) {
    if (error_message) {
      *error_message = "摄像头模块收到无效帧";
    }
    return false;
  }

  rtc_camera_headless::ConvertedI420Frame converted;
  std::string convert_error;
  if (!converter_->ConvertToI420(frame, &converted, &convert_error)) {
    if (error_message) {
      *error_message = std::string("摄像头格式转换失败：") + convert_error;
    }
    return false;
  }
  if (!rtc_session->SendI420Frame(
          kStereoCameraVideoSourceId, converted.data, converted.data_size,
          converted.width,
          converted.height, converted.stride_y, converted.stride_u,
          converted.stride_v)) {
    if (error_message) {
      *error_message = "RTC 视频帧发送失败";
    }
    return false;
  }
  return true;
}

uint64_t DualCameraStreamingModule::captured_frames() const {
  return video_source_ ? video_source_->captured_frames() : 0;
}

}  // 命名空间 rtc_edge
