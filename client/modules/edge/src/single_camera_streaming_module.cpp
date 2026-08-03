#include "rtc_edge/single_camera_streaming_module.h"

#include "rtc_camera/single/frame_converter.h"
#include "rtc_logging/rtc_logging.h"
#include "rtc_runtime/rtc_session.h"

#include <stdexcept>
#include <thread>

namespace rtc_edge {
namespace {

bool IsV4l2DevicePath(const std::string& path) {
  return path.find("/dev/") == 0;
}

bool IsAbsolutePath(const std::string& path) {
  return !path.empty() &&
         (path[0] == '/' || (path.size() > 2 && path[1] == ':'));
}

std::string JoinPath(const std::string& base, const std::string& leaf) {
  if (base.empty() || base == ".") {
    return leaf;
  }
  const char tail = base[base.size() - 1];
  if (tail == '/' || tail == '\\') {
    return base + leaf;
  }
  return base + "/" + leaf;
}

}  // namespace

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
  if (!IsV4l2DevicePath(options_.capture.device)) {
    return StartLocalYuvFile(error_message);
  }

  try {
    video_source_.reset(
        new rtc_camera::single::AsyncCameraImageSource(options_.capture));
    rtc_frames_ = video_source_->Subscribe(1);
    if (!rtc_frames_) {
      if (error_message != nullptr) {
        *error_message = "Failed to create camera frame subscription";
      }
      video_source_.reset();
      return false;
    }

    converter_.reset(new rtc_camera::single::CameraFrameConverter());
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
  if (local_yuv_file_.is_open()) {
    local_yuv_file_.close();
  }
  local_i420_frame_.clear();
  local_frame_size_ = 0;
  local_width_ = 0;
  local_height_ = 0;
  local_stride_y_ = 0;
  local_stride_u_ = 0;
  local_stride_v_ = 0;
  local_captured_frames_ = 0;
  use_local_yuv_file_ = false;
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
    bool send_frame,
    std::string* error_message) {
  if (use_local_yuv_file_) {
    return TickLocalYuvFile(rtc_session, send_frame, error_message);
  }
  if (!started_ || rtc_session == nullptr || !video_source_ || !rtc_frames_ ||
      !converter_) {
    if (error_message != nullptr) {
      *error_message = "Single camera module has not been started";
    }
    return false;
  }

  rtc_camera::single::CameraFrame frame;
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
  if (!send_frame) {
    return true;
  }

  rtc_camera::single::ConvertedCameraFrame converted;
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

bool SingleCameraStreamingModule::StartLocalYuvFile(
    std::string* error_message) {
  if (options_.capture.width <= 0 || options_.capture.height <= 0 ||
      (options_.capture.width % 2) != 0 || (options_.capture.height % 2) != 0) {
    if (error_message != nullptr) {
      *error_message = options_.camera_name +
                       " local YUV width and height must be positive even values";
    }
    return false;
  }

  local_width_ = static_cast<size_t>(options_.capture.width);
  local_height_ = static_cast<size_t>(options_.capture.height);
  local_stride_y_ = local_width_;
  local_stride_u_ = local_width_ / 2;
  local_stride_v_ = local_width_ / 2;
  local_frame_size_ = local_width_ * local_height_ * 3 / 2;
  local_i420_frame_.assign(local_frame_size_, 0);

  local_yuv_file_.open(options_.capture.device.c_str(), std::ios::binary);
  if (!local_yuv_file_.is_open() &&
      !IsAbsolutePath(options_.capture.device)) {
    std::string base = ".";
    for (int i = 0; i < 6 && !local_yuv_file_.is_open(); ++i) {
      const std::string candidate = JoinPath(base, options_.capture.device);
      local_yuv_file_.clear();
      local_yuv_file_.open(candidate.c_str(), std::ios::binary);
      base = JoinPath(base, "..");
    }
  }
  if (!local_yuv_file_.is_open()) {
    if (error_message != nullptr) {
      *error_message = options_.camera_name +
                       " local YUV file cannot be opened: " +
                       options_.capture.device;
    }
    return false;
  }
  local_captured_frames_ = 0;
  use_local_yuv_file_ = true;
  started_ = true;
  first_frame_logged_ = false;
  return true;
}

bool SingleCameraStreamingModule::TickLocalYuvFile(
    rtc_runtime::RtcSession* rtc_session,
    bool send_frame,
    std::string* error_message) {
  if (!started_ || rtc_session == nullptr || !local_yuv_file_.is_open() ||
      local_i420_frame_.empty()) {
    if (error_message != nullptr) {
      *error_message = "Local YUV module has not been started";
    }
    return false;
  }

  std::this_thread::sleep_for(options_.frame_wait);
  local_yuv_file_.read(reinterpret_cast<char*>(local_i420_frame_.data()),
                       static_cast<std::streamsize>(local_frame_size_));
  if (local_yuv_file_.gcount() !=
      static_cast<std::streamsize>(local_frame_size_)) {
    local_yuv_file_.clear();
    local_yuv_file_.seekg(0, std::ios::beg);
    local_yuv_file_.read(reinterpret_cast<char*>(local_i420_frame_.data()),
                         static_cast<std::streamsize>(local_frame_size_));
    if (local_yuv_file_.gcount() !=
        static_cast<std::streamsize>(local_frame_size_)) {
      if (error_message != nullptr) {
        *error_message = options_.camera_name +
                         " local YUV file is smaller than one I420 frame";
      }
      return false;
    }
  }

  rtc_session->NoteCapturedFrame();
  ++local_captured_frames_;
  if (!first_frame_logged_) {
    first_frame_logged_ = true;
    rtc_logging::LogInfo(options_.camera_name +
                         " loaded local YUV frame");
  }
  if (!rtc_session->IsReadyToSend() || !send_frame) {
    return true;
  }

  if (!rtc_session->SendI420Frame(
          options_.video_source_id.c_str(), local_i420_frame_.data(),
          local_i420_frame_.size(), local_width_, local_height_,
          local_stride_y_, local_stride_u_, local_stride_v_)) {
    if (error_message != nullptr) {
      *error_message =
          options_.camera_name + " local YUV RTC frame send failed";
    }
    return false;
  }
  return true;
}

bool SingleCameraStreamingModule::started() const {
  return started_;
}

uint64_t SingleCameraStreamingModule::captured_frames() const {
  if (use_local_yuv_file_) {
    return local_captured_frames_;
  }
  return video_source_ ? video_source_->captured_frames() : 0;
}

}  // 命名空间 rtc_edge
