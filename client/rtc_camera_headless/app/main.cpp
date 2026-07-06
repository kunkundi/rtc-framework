#include "rtc_camera_common.h"
#include "rtc_headless_session.h"
#include "uyvy_to_i420_cuda.h"

using namespace rtc_camera_headless;

#ifdef VTSRTC_USE_MIIVII_SDK
#include "mv_gmsl_camera.h"
using CaptureDevice = MvGmslCaptureDevice;
#else
#include "uyvy_v4l2_camera.h"
using CaptureDevice = UyvyV4l2CaptureDevice;
#endif

#include <linux/videodev2.h>
#include <chrono>
#include <cstring>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

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

// Convert NV12 to I420 on CPU.
// NV12 layout: Y plane (stride*height bytes) + interleaved UV plane (stride*height/2 bytes).
// I420 layout: Y plane + U plane + V plane (all contiguous).
bool ConvertNv12ToI420OnCpu(const uint8_t* nv12_data,
                            size_t nv12_size,
                            size_t width,
                            size_t height,
                            size_t stride_bytes,
                            std::vector<uint8_t>* i420_out,
                            size_t* y_stride,
                            size_t* u_stride,
                            size_t* v_stride) {
  if (!nv12_data || !i420_out) {
    return false;
  }

  const size_t y_plane_size = stride_bytes * height;
  const size_t uv_plane_size = stride_bytes * (height / 2);
  if (nv12_size < y_plane_size + uv_plane_size) {
    return false;
  }

  const size_t out_y_stride = width;
  const size_t out_u_stride = width / 2;
  const size_t out_v_stride = width / 2;
  const size_t chroma_height = (height + 1) / 2;
  const size_t out_size =
      out_y_stride * height + out_u_stride * chroma_height * 2;

  i420_out->resize(out_size);
  uint8_t* dst_y = i420_out->data();
  uint8_t* dst_u = dst_y + out_y_stride * height;
  uint8_t* dst_v = dst_u + out_u_stride * chroma_height;

  // Copy Y plane row-by-row (handles stride > width)
  for (size_t row = 0; row < height; ++row) {
    std::memcpy(dst_y + row * out_y_stride,
                nv12_data + row * stride_bytes, width);
  }

  // Deinterleave UV plane
  const uint8_t* uv_src = nv12_data + y_plane_size;
  for (size_t row = 0; row < chroma_height; ++row) {
    for (size_t col = 0; col < width / 2; ++col) {
      const size_t src_off = row * stride_bytes + col * 2;
      const size_t dst_off = row * out_u_stride + col;
      dst_u[dst_off] = uv_src[src_off];
      dst_v[dst_off] = uv_src[src_off + 1];
    }
  }

  if (y_stride) *y_stride = out_y_stride;
  if (u_stride) *u_stride = out_u_stride;
  if (v_stride) *v_stride = out_v_stride;
  return true;
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

        if (!rtc_session.IsReadyToSend()) {
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

    CaptureDevice capture_device;
    capture_device.Open(camera_options);
    LogInfo(std::string("camera: ") + capture_device.device_path() + " " +
            std::to_string(capture_device.width()) + "x" +
            std::to_string(capture_device.height()) + " " +
            FourccToString(capture_device.pixel_format()));

    const bool camera_is_nv12 = capture_device.is_nv12();
    const bool camera_is_yuyv =
        capture_device.pixel_format() == V4L2_PIX_FMT_YUYV;

    // CUDA converter for UYVY / YUYV cameras.
    std::unique_ptr<UyvyToI420CudaConverter> converter;
    if (!camera_is_nv12) {
      converter.reset(new UyvyToI420CudaConverter());
      std::string cuda_error;
      if (!converter->Init(capture_device.width(), capture_device.height(),
                           capture_device.bytes_per_line(), camera_is_yuyv,
                           &cuda_error)) {
        throw std::runtime_error("failed to init CUDA UYVY converter: " +
                                 cuda_error);
      }
      LogInfo(std::string("using CUDA ") +
              (camera_is_yuyv ? "YUYV" : "UYVY") + "->I420 converter");
    } else {
      LogInfo("using CPU NV12->I420 converter");
    }

    // NV12 intermediate buffers.
    std::vector<uint8_t> nv12_i420_buffer;
    size_t nv12_y_stride = 0;
    size_t nv12_u_stride = 0;
    size_t nv12_v_stride = 0;

    RunWarmup(capture_device, options);

    RtcHeadlessSession rtc_session(options);
    if (!rtc_session.Init()) {
      return 1;
    }

    CaptureDevice::CapturedFrame raw_frame;
    std::string cuda_error;
    bool first_frame_logged = false;
    bool i420_dumped = false;

    while (!StopRequested()) {
      rtc_session.Tick();

      if (!capture_device.DequeueCapturedFrame(&raw_frame)) {
        continue;
      }

      rtc_session.NoteCapturedFrame();
      if (!first_frame_logged) {
        first_frame_logged = true;
        LogInfo("first camera frame captured");
        // Dump raw camera frame for debugging
        {
          std::ofstream dump("first_frame_raw.bin", std::ios::binary);
          dump.write(reinterpret_cast<const char*>(raw_frame.data),
                     static_cast<std::streamsize>(raw_frame.bytes_used));
          LogInfo("dumped raw frame to first_frame_raw.bin (" +
                  std::to_string(raw_frame.bytes_used) + " bytes)");
        }
      }

      if (!rtc_session.IsReadyToSend()) {
        capture_device.RequeueCapturedFrame(&raw_frame);
        continue;
      }

      bool send_ok = false;
      if (camera_is_nv12) {
        // NV12 → I420 CPU conversion
        if (!ConvertNv12ToI420OnCpu(
                raw_frame.data, raw_frame.bytes_used,
                capture_device.width(), capture_device.height(),
                capture_device.bytes_per_line(), &nv12_i420_buffer,
                &nv12_y_stride, &nv12_u_stride, &nv12_v_stride)) {
          capture_device.RequeueCapturedFrame(&raw_frame);
          throw std::runtime_error("failed to convert NV12 to I420 on CPU");
        }
        capture_device.RequeueCapturedFrame(&raw_frame);

        // Dump first converted I420 for debugging
        if (!i420_dumped) {
          i420_dumped = true;
          std::ofstream dump("first_frame_i420.yuv", std::ios::binary);
          dump.write(reinterpret_cast<const char*>(nv12_i420_buffer.data()),
                     static_cast<std::streamsize>(nv12_i420_buffer.size()));
          LogInfo("dumped I420 frame to first_frame_i420.yuv (" +
                  std::to_string(nv12_i420_buffer.size()) + " bytes)");
        }

        send_ok = rtc_session.SendI420Frame(
            nv12_i420_buffer.data(), nv12_i420_buffer.size(),
            capture_device.width(), capture_device.height(),
            nv12_y_stride, nv12_u_stride, nv12_v_stride);
      } else {
        // UYVY / YUYV → I420 CUDA conversion
        const uint8_t* i420_data = nullptr;
        size_t i420_size = 0;
        if (!converter->Convert(raw_frame.data, raw_frame.bytes_used,
                               &i420_data, &i420_size, &cuda_error)) {
          capture_device.RequeueCapturedFrame(&raw_frame);
          throw std::runtime_error(
              "failed to convert UYVY to I420 on CUDA: " + cuda_error);
        }
        capture_device.RequeueCapturedFrame(&raw_frame);

        // Dump first converted I420 for debugging
        if (!i420_dumped) {
          i420_dumped = true;
          std::ofstream dump("first_frame_i420.yuv", std::ios::binary);
          dump.write(reinterpret_cast<const char*>(i420_data),
                     static_cast<std::streamsize>(i420_size));
          LogInfo("dumped I420 frame to first_frame_i420.yuv (" +
                  std::to_string(i420_size) + " bytes)");
        }

        send_ok = rtc_session.SendI420Frame(
            i420_data, i420_size, capture_device.width(),
            capture_device.height(), converter->y_stride(),
            converter->u_stride(), converter->v_stride());
      }

      if (!send_ok) {
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
