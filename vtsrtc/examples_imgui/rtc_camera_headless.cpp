#include "rtc_camera_common.h"
#include "rtc_headless_session.h"
#include "uyvy_to_i420_cuda.h"
#include "uyvy_v4l2_camera.h"

#include <chrono>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace rtc_camera_headless;

namespace {

constexpr size_t kYuv420Width = 1280;
constexpr size_t kYuv420Height = 720;
constexpr auto kYuv420FrameInterval = std::chrono::milliseconds(33);

bool EndsWith(const std::string& value, const std::string& suffix) {
  if (value.size() < suffix.size()) {
    return false;
  }
  return value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

class Yuv420FileSource {
 public:
  void Open(const std::string& path, size_t width, size_t height) {
    path_ = path;
    width_ = width == 0 ? kYuv420Width : width;
    height_ = height == 0 ? kYuv420Height : height;

    if (width_ != kYuv420Width || height_ != kYuv420Height) {
      throw std::runtime_error(
          "zjlabs.yuv only supports 1280x720 YUV420p input");
    }

    frame_size_ = width_ * height_ * 3 / 2;

    file_.open(path_.c_str(), std::ios::binary);
    if (!file_.is_open()) {
      throw std::runtime_error("failed to open YUV file: " + path_);
    }

    file_.seekg(0, std::ios::end);
    const std::streamoff file_size = file_.tellg();
    if (file_size < 0) {
      throw std::runtime_error("failed to determine YUV file size: " + path_);
    }

    const size_t byte_count = static_cast<size_t>(file_size);
    if (byte_count == 0 || (byte_count % frame_size_) != 0) {
      throw std::runtime_error("invalid YUV file size for " + path_ +
                               ": expected a multiple of " +
                               std::to_string(frame_size_) + " bytes");
    }

    frame_count_ = byte_count / frame_size_;
    frames_read_ = 0;
    file_.clear();
    file_.seekg(0, std::ios::beg);
  }

  bool DequeueRawFrame(std::vector<uint8_t>* raw_frame, size_t* bytes_used) {
    if (!raw_frame) {
      throw std::runtime_error("raw_frame output buffer is null");
    }

    raw_frame->resize(frame_size_);
    file_.read(reinterpret_cast<char*>(raw_frame->data()),
               static_cast<std::streamsize>(frame_size_));
    if (!file_) {
      throw std::runtime_error("failed to read YUV frame from: " + path_);
    }

    if (bytes_used) {
      *bytes_used = frame_size_;
    }

    ++frames_read_;
    if (frames_read_ >= frame_count_) {
      file_.clear();
      file_.seekg(0, std::ios::beg);
      frames_read_ = 0;
    }

    return true;
  }

  size_t width() const {
    return width_;
  }

  size_t height() const {
    return height_;
  }

  size_t y_stride() const {
    return width_;
  }

  size_t u_stride() const {
    return width_ / 2;
  }

  size_t v_stride() const {
    return width_ / 2;
  }

  size_t frame_count() const {
    return frame_count_;
  }

  const std::string& path() const {
    return path_;
  }

 private:
  std::ifstream file_;
  std::string path_;
  size_t width_ = 0;
  size_t height_ = 0;
  size_t frame_size_ = 0;
  size_t frame_count_ = 0;
  size_t frames_read_ = 0;
};

bool MatchesBundledYuvResolution(const CaptureOptions& options) {
  const bool width_ok = options.width == 0 || options.width == 1280;
  const bool height_ok = options.height == 0 || options.height == 720;
  return width_ok && height_ok;
}

void RunWarmup(Yuv420FileSource& source, const CaptureOptions& options) {
  if (options.warmup_delay_ms > 0) {
    std::this_thread::sleep_for(
        std::chrono::milliseconds(options.warmup_delay_ms));
  }
  if (options.warmup_frames <= 0) {
    return;
  }

  std::vector<uint8_t> discard_frame;
  size_t bytes_used = 0;
  int discarded = 0;
  while (!StopRequested() && discarded < options.warmup_frames) {
    if (!source.DequeueRawFrame(&discard_frame, &bytes_used)) {
      break;
    }
    ++discarded;
  }
}

}  // namespace

int main(int argc, char** argv) {
  InstallSignalHandlers();

  try {
    const CaptureOptions options = ParseArgs(argc, argv);

    const bool explicit_yuv_file =
        !options.device.empty() && EndsWith(options.device, ".yuv");
    const std::string bundled_yuv_path = ResolveConfigPath("zjlabs.yuv");
    const bool use_bundled_yuv =
        options.device.empty() && MatchesBundledYuvResolution(options) &&
        FileExists(bundled_yuv_path);

    if (explicit_yuv_file || use_bundled_yuv) {
      const std::string video_path = explicit_yuv_file ? options.device
                                                       : bundled_yuv_path;

      if (explicit_yuv_file && !FileExists(video_path)) {
        throw std::runtime_error("failed to locate YUV file: " + video_path);
      }

      Yuv420FileSource capture_source;
      capture_source.Open(video_path, options.width, options.height);
      LogInfo(std::string("video file: ") + capture_source.path() + " " +
              std::to_string(capture_source.width()) + "x" +
              std::to_string(capture_source.height()) + " frames=" +
              std::to_string(capture_source.frame_count()));

      RunWarmup(capture_source, options);

      RtcHeadlessSession rtc_session(options);
      if (!rtc_session.Init()) {
        return 1;
      }

      std::vector<uint8_t> raw_frame;
      size_t raw_bytes_used = 0;
      bool first_frame_logged = false;

      while (!StopRequested()) {
        rtc_session.Tick();

        if (!rtc_session.IsRoomJoined()) {
          std::this_thread::sleep_for(std::chrono::milliseconds(50));
          continue;
        }

        if (!capture_source.DequeueRawFrame(&raw_frame, &raw_bytes_used)) {
          continue;
        }

        rtc_session.NoteCapturedFrame();
        if (!first_frame_logged) {
          first_frame_logged = true;
          LogInfo("first video frame captured");
        }

        if (!rtc_session.SendI420Frame(
                raw_frame.data(), raw_bytes_used, capture_source.width(),
                capture_source.height(), capture_source.y_stride(),
                capture_source.u_stride(), capture_source.v_stride())) {
          throw std::runtime_error("failed to send I420 frame");
        }

        if (options.frame_limit > 0 &&
            rtc_session.sent_frames() >=
                static_cast<uint64_t>(options.frame_limit)) {
          LogInfo("frame limit reached");
          RequestStop();
          break;
        }

        std::this_thread::sleep_for(kYuv420FrameInterval);
      }

      return 0;
    }

    const std::string camera_device_path =
        options.device.empty() ? "/dev/video0" : options.device;
    CaptureOptions camera_options = options;
    camera_options.device = camera_device_path;

    UyvyV4l2CaptureDevice capture_device;
    capture_device.Open(camera_options);
    LogInfo(std::string("camera: ") + capture_device.device_path() + " " +
            std::to_string(capture_device.width()) + "x" +
            std::to_string(capture_device.height()) + " " +
            FourccToString(capture_device.pixel_format()));

    UyvyToI420CudaConverter converter;
    std::string cuda_error;
    if (!converter.Init(capture_device.width(), capture_device.height(),
                        capture_device.bytes_per_line(), &cuda_error)) {
      throw std::runtime_error("failed to init CUDA UYVY converter: " +
                               cuda_error);
    }
    LogInfo("using CUDA UYVY->I420 converter");

    RunWarmup(capture_device, options);

    RtcHeadlessSession rtc_session(options);
    if (!rtc_session.Init()) {
      return 1;
    }

    std::vector<uint8_t> raw_frame;
    size_t raw_bytes_used = 0;
    bool first_frame_logged = false;

    while (!StopRequested()) {
      rtc_session.Tick();

      if (!capture_device.DequeueRawFrame(&raw_frame, &raw_bytes_used)) {
        continue;
      }

      rtc_session.NoteCapturedFrame();
      if (!first_frame_logged) {
        first_frame_logged = true;
        LogInfo("first camera frame captured");
      }

      if (!rtc_session.IsRoomJoined()) {
        continue;
      }

      const uint8_t* i420_data = nullptr;
      size_t i420_size = 0;
      if (!converter.Convert(raw_frame.data(), raw_bytes_used, &i420_data,
                             &i420_size, &cuda_error)) {
        throw std::runtime_error("failed to convert UYVY to I420 on CUDA: " +
                                 cuda_error);
      }

      if (!rtc_session.SendI420Frame(
          i420_data, i420_size, capture_device.width(),
          capture_device.height(), converter.y_stride(),
          converter.u_stride(), converter.v_stride())) {
        throw std::runtime_error("failed to send I420 frame");
      }

      if (options.frame_limit > 0 &&
          rtc_session.sent_frames() >= static_cast<uint64_t>(options.frame_limit)) {
        LogInfo("frame limit reached");
        RequestStop();
        break;
      }
    }

    return 0;
  } catch (const std::exception& ex) {
    LogError(std::string("rtc_camera_headless failed: ") + ex.what());
    return 1;
  }
}
