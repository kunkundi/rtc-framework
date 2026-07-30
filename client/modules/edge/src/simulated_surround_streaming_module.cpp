#include "rtc_edge/simulated_surround_streaming_module.h"

#include "rtc_edge/camera_video_sources.h"
#include "rtc_logging/rtc_logging.h"
#include "rtc_runtime/rtc_session.h"

#include <algorithm>
#include <limits>
#include <thread>

namespace rtc_edge {
namespace {

enum PlaneTransform {
  kNone,
  kMirrorHorizontal,
  kMirrorVertical,
  kRotate180,
};

void TransformPlane(const uint8_t* source,
                    size_t plane_width,
                    size_t plane_height,
                    uint8_t* destination,
                    PlaneTransform transform) {
  for (size_t y = 0; y < plane_height; ++y) {
    for (size_t x = 0; x < plane_width; ++x) {
      size_t source_x = x;
      size_t source_y = y;
      if (transform == kMirrorHorizontal || transform == kRotate180) {
        source_x = plane_width - 1 - source_x;
      }
      if (transform == kMirrorVertical || transform == kRotate180) {
        source_y = plane_height - 1 - source_y;
      }
      destination[y * plane_width + x] =
          source[source_y * plane_width + source_x];
    }
  }
}

}  // namespace

SimulatedSurroundStreamingModule::SimulatedSurroundStreamingModule(
    const SimulatedSurroundStreamingModuleOptions& options)
    : options_(options) {}

SimulatedSurroundStreamingModule::~SimulatedSurroundStreamingModule() {
  Stop();
}

bool SimulatedSurroundStreamingModule::Start(std::string* error_message) {
  if (started_) {
    return true;
  }
  if (options_.yuv_path.empty()) {
    if (error_message != nullptr) {
      *error_message = "Simulated surround YUV path must not be empty";
    }
    return false;
  }
  if (options_.width == 0 || options_.height == 0 ||
      options_.width % 2 != 0 || options_.height % 2 != 0) {
    if (error_message != nullptr) {
      *error_message =
          "Simulated surround width and height must be divisible by 2";
    }
    return false;
  }
  if (options_.fps <= 0) {
    if (error_message != nullptr) {
      *error_message = "Simulated surround FPS must be positive";
    }
    return false;
  }
  if (options_.width > std::numeric_limits<size_t>::max() /
                           options_.height) {
    if (error_message != nullptr) {
      *error_message = "Simulated surround frame size is too large";
    }
    return false;
  }

  const size_t luma_size = options_.width * options_.height;
  if (luma_size > std::numeric_limits<size_t>::max() / 3 * 2) {
    if (error_message != nullptr) {
      *error_message = "Simulated surround frame size is too large";
    }
    return false;
  }
  const size_t frame_size = luma_size * 3 / 2;
  if (frame_size >
      static_cast<size_t>(std::numeric_limits<std::streamsize>::max())) {
    if (error_message != nullptr) {
      *error_message = "Simulated surround frame size is too large";
    }
    return false;
  }
  input_frame_.resize(frame_size);
  transformed_frame_.resize(frame_size);

  input_.open(options_.yuv_path.c_str(), std::ios::binary);
  if (!input_.is_open()) {
    if (error_message != nullptr) {
      *error_message = "Failed to open simulated surround YUV file: " +
                       options_.yuv_path;
    }
    Stop();
    return false;
  }
  if (!ReadNextFrame(error_message)) {
    Stop();
    return false;
  }

  frame_period_ = std::chrono::microseconds(1000000 / options_.fps);
  next_frame_time_ = std::chrono::steady_clock::now();
  started_ = true;
  first_frame_logged_ = false;
  return true;
}

void SimulatedSurroundStreamingModule::Stop() {
  if (input_.is_open()) {
    input_.close();
  }
  input_.clear();
  input_frame_.clear();
  transformed_frame_.clear();
  started_ = false;
  first_frame_logged_ = false;
}

bool SimulatedSurroundStreamingModule::Tick(
    rtc_runtime::RtcSession* rtc_session,
    std::string* error_message) {
  if (!started_ || rtc_session == nullptr) {
    if (error_message != nullptr) {
      *error_message = "Simulated surround streaming module is not started";
    }
    return false;
  }

  const std::chrono::steady_clock::time_point now =
      std::chrono::steady_clock::now();
  if (now < next_frame_time_) {
    const std::chrono::steady_clock::duration remaining =
        next_frame_time_ - now;
    const std::chrono::steady_clock::duration maximum_sleep =
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::milliseconds(1));
    std::this_thread::sleep_for(
        remaining < maximum_sleep ? remaining : maximum_sleep);
    return true;
  }

  rtc_session->NoteCapturedFrame();
  if (!first_frame_logged_) {
    first_frame_logged_ = true;
    rtc_logging::LogInfo(
        "Simulated surround module produced its first four-source frame set");
  }
  if (rtc_session->IsReadyToSend()) {
    if (!SendSimulatedCamera(rtc_session, kSurroundFrontVideoSourceId, kNone,
                             error_message) ||
        !SendSimulatedCamera(rtc_session, kSurroundRearVideoSourceId,
                             kRotate180, error_message) ||
        !SendSimulatedCamera(rtc_session, kSurroundLeftVideoSourceId,
                             kMirrorHorizontal, error_message) ||
        !SendSimulatedCamera(rtc_session, kSurroundRightVideoSourceId,
                             kMirrorVertical, error_message)) {
      return false;
    }
  }

  if (!ReadNextFrame(error_message)) {
    return false;
  }

  next_frame_time_ += frame_period_;
  if (next_frame_time_ <= now) {
    next_frame_time_ = now + frame_period_;
  }
  return true;
}

bool SimulatedSurroundStreamingModule::ReadNextFrame(
    std::string* error_message) {
  const std::streamsize expected =
      static_cast<std::streamsize>(input_frame_.size());
  input_.read(reinterpret_cast<char*>(input_frame_.data()), expected);
  if (input_.gcount() == expected) {
    return true;
  }

  input_.clear();
  input_.seekg(0, std::ios::beg);
  input_.read(reinterpret_cast<char*>(input_frame_.data()), expected);
  if (input_.gcount() == expected) {
    return true;
  }

  if (error_message != nullptr) {
    *error_message =
        "Simulated surround YUV file does not contain one complete frame";
  }
  return false;
}

bool SimulatedSurroundStreamingModule::SendSimulatedCamera(
    rtc_runtime::RtcSession* rtc_session,
    const char* source_id,
    int transform,
    std::string* error_message) {
  TransformFrame(transform);
  if (!rtc_session->SendI420Frame(
          source_id, transformed_frame_.data(), transformed_frame_.size(),
          options_.width, options_.height, options_.width, options_.width / 2,
          options_.width / 2)) {
    if (error_message != nullptr) {
      *error_message = std::string("Failed to send simulated surround RTC "
                                   "video frame: ") + source_id;
    }
    return false;
  }
  return true;
}

void SimulatedSurroundStreamingModule::TransformFrame(int transform) {
  const size_t luma_size = options_.width * options_.height;
  const size_t chroma_width = options_.width / 2;
  const size_t chroma_height = options_.height / 2;
  const size_t chroma_size = chroma_width * chroma_height;

  const uint8_t* input_y = input_frame_.data();
  const uint8_t* input_u = input_y + luma_size;
  const uint8_t* input_v = input_u + chroma_size;
  uint8_t* output_y = transformed_frame_.data();
  uint8_t* output_u = output_y + luma_size;
  uint8_t* output_v = output_u + chroma_size;

  const PlaneTransform plane_transform =
      static_cast<PlaneTransform>(transform);
  TransformPlane(input_y, options_.width, options_.height, output_y,
                 plane_transform);
  TransformPlane(input_u, chroma_width, chroma_height, output_u,
                 plane_transform);
  TransformPlane(input_v, chroma_width, chroma_height, output_v,
                 plane_transform);
}

}  // namespace rtc_edge
