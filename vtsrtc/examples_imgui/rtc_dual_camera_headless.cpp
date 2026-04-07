#include "rtc_camera_common.h"
#include "dual_uyvy_to_i420_stitch_cuda.h"
#include "rtc_headless_session.h"
#include "uyvy_v4l2_camera.h"

#include <stddef.h>
#include <stdint.h>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace rtc_camera_headless;

namespace {

constexpr const char* kDefaultLeftDevice = "/dev/video0";
constexpr const char* kDefaultRightDevice = "/dev/video1";
struct DualCaptureOptions {
  CaptureOptions rtc_options;
  std::string left_device = kDefaultLeftDevice;
  std::string right_device = kDefaultRightDevice;
};

class CapturedFrameGuard {
 public:
  CapturedFrameGuard(UyvyV4l2CaptureDevice* device,
                     UyvyV4l2CaptureDevice::CapturedFrame* frame)
      : device_(device), frame_(frame) {}

  ~CapturedFrameGuard() {
    if (device_ && frame_ && frame_->data) {
      device_->RequeueCapturedFrame(frame_);
    }
  }

  CapturedFrameGuard(const CapturedFrameGuard&) = delete;
  CapturedFrameGuard& operator=(const CapturedFrameGuard&) = delete;

 private:
  UyvyV4l2CaptureDevice* device_ = nullptr;
  UyvyV4l2CaptureDevice::CapturedFrame* frame_ = nullptr;
};

bool CommonOptionTakesValue(const std::string& arg) {
  return arg == "--device" || arg == "--room" || arg == "--config" ||
         arg == "--width" || arg == "--height" || arg == "--frame-limit" ||
         arg == "--buffer-count" || arg == "--timeout-ms" ||
         arg == "--warmup-frames" || arg == "--warmup-delay-ms" ||
         arg == "--join-retry-ms" || arg == "--status-interval-sec";
}

void PrintDualUsage(const char* program) {
  std::cout
      << "Usage: " << program << " [options]\n"
      << "\n"
      << "Headless dual-camera RTC client. Captures two UYVY cameras,\n"
      << "converts both streams to I420, samples every other horizontal pixel\n"
      << "to halve each camera width, stitches them side-by-side, and sends\n"
      << "the merged frame through the external RTC video source.\n"
      << "\n"
      << "Local options:\n"
      << "  --left-device /dev/video0   Left camera node\n"
      << "  --right-device /dev/video1  Right camera node\n"
      << "\n"
      << "Shared options:\n"
      << "  --room zhejianglab         Room to auto join after RTC login\n"
      << "  --config rtc.cfg           RTC config path, defaults to nearby rtc.cfg\n"
      << "  --width 1280               Requested capture width for both cameras\n"
      << "  --height 720               Requested capture height for both cameras\n"
      << "  --buffer-count 4           Number of mmap capture buffers per camera\n"
      << "  --timeout-ms 2000          Poll timeout while waiting for frames\n"
      << "  --warmup-frames 0          Discard N stitched frame pairs after open\n"
      << "  --warmup-delay-ms 0        Sleep once before warmup after open\n"
      << "  --join-retry-ms 3000       Retry interval for auto join\n"
      << "  --status-interval-sec 5    Status log interval, 0 disables periodic logs\n"
      << "  --frame-limit 0            Exit after sending N stitched RTC frames\n"
      << "  --help                     Show this message\n"
      << "\n"
      << "Notes:\n"
      << "  Use --left-device and --right-device here; --device is not supported.\n"
      << "\n"
      << "Examples:\n"
      << "  " << program << "\n"
      << "  " << program << " --width 1280 --height 720\n"
      << "  " << program
      << " --left-device /dev/video2 --right-device /dev/video3 --room zhejianglab\n"
      << std::endl;
}

DualCaptureOptions ParseDualArgs(int argc, char** argv) {
  DualCaptureOptions options;

  std::vector<char*> forwarded_args;
  forwarded_args.reserve(static_cast<size_t>(argc));
  forwarded_args.push_back(argv[0]);

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto require_value = [&](const char* name) -> std::string {
      if (i + 1 >= argc) {
        throw std::runtime_error(std::string("missing value for ") + name);
      }
      ++i;
      return argv[i];
    };

    if (arg == "--help" || arg == "-h") {
      PrintDualUsage(argv[0]);
      std::exit(0);
    }

    if (arg == "--left-device") {
      options.left_device = require_value("--left-device");
      continue;
    }
    if (arg == "--right-device") {
      options.right_device = require_value("--right-device");
      continue;
    }

    forwarded_args.push_back(argv[i]);
    if (CommonOptionTakesValue(arg)) {
      if (i + 1 >= argc) {
        throw std::runtime_error(std::string("missing value for ") +
                                 arg);
      }
      forwarded_args.push_back(argv[++i]);
    }
  }

  options.rtc_options =
      ParseArgs(static_cast<int>(forwarded_args.size()), forwarded_args.data());
  if (!options.rtc_options.device.empty()) {
    throw std::runtime_error(
        "--device is not supported in the dual-camera client, use "
        "--left-device/--right-device instead");
  }

  return options;
}

void LogCameraInfo(const char* label, const UyvyV4l2CaptureDevice& device) {
  LogInfo(std::string(label) + ": " + device.device_path() + " " +
          std::to_string(device.width()) + "x" +
          std::to_string(device.height()) + " " +
          FourccToString(device.pixel_format()));
}

void RunDualWarmup(UyvyV4l2CaptureDevice& left_device,
                   UyvyV4l2CaptureDevice& right_device,
                   const CaptureOptions& options) {
  if (options.warmup_delay_ms > 0) {
    std::this_thread::sleep_for(
        std::chrono::milliseconds(options.warmup_delay_ms));
  }
  if (options.warmup_frames <= 0) {
    return;
  }

  int discarded = 0;
  while (!StopRequested() && discarded < options.warmup_frames) {
    UyvyV4l2CaptureDevice::CapturedFrame left_frame;
    if (!left_device.DequeueCapturedFrame(&left_frame)) {
      continue;
    }
    CapturedFrameGuard left_guard(&left_device, &left_frame);

    UyvyV4l2CaptureDevice::CapturedFrame right_frame;
    if (!right_device.DequeueCapturedFrame(&right_frame)) {
      continue;
    }
    CapturedFrameGuard right_guard(&right_device, &right_frame);
    ++discarded;
  }
}

}  // namespace

int main(int argc, char** argv) {
  InstallSignalHandlers();

  try {
    const DualCaptureOptions dual_options = ParseDualArgs(argc, argv);
    const CaptureOptions& options = dual_options.rtc_options;

    CaptureOptions left_camera_options = options;
    left_camera_options.device = dual_options.left_device;

    UyvyV4l2CaptureDevice left_device;
    left_device.Open(left_camera_options);
    LogCameraInfo("left camera", left_device);

    CaptureOptions right_camera_options = options;
    right_camera_options.device = dual_options.right_device;
    if (right_camera_options.width == 0) {
      right_camera_options.width = static_cast<int>(left_device.width());
    }
    if (right_camera_options.height == 0) {
      right_camera_options.height = static_cast<int>(left_device.height());
    }

    UyvyV4l2CaptureDevice right_device;
    right_device.Open(right_camera_options);
    LogCameraInfo("right camera", right_device);

    if (left_device.height() != right_device.height()) {
      throw std::runtime_error(
          "camera heights do not match, unable to stitch side-by-side");
    }
    DualUyvyToI420StitchCudaConverter converter;
    std::string cuda_error;
    if (!converter.Init(left_device.width(), left_device.height(),
                        left_device.bytes_per_line(), right_device.width(),
                        right_device.height(), right_device.bytes_per_line(),
                        &cuda_error)) {
      throw std::runtime_error(
          "failed to init CUDA dual-camera stitch converter: " + cuda_error);
    }
    LogInfo("using CUDA dual-camera UYVY->I420 stitch converter");

    const size_t stitched_width = converter.output_width();
    const size_t stitched_height = converter.output_height();
    LogInfo(std::string("sampled left output: ") +
            std::to_string(left_device.width() / 2) + "x" +
            std::to_string(left_device.height()));
    LogInfo(std::string("sampled right output: ") +
            std::to_string(right_device.width() / 2) + "x" +
            std::to_string(right_device.height()));
    LogInfo(std::string("stitched output: ") + std::to_string(stitched_width) +
            "x" + std::to_string(stitched_height));

    RunDualWarmup(left_device, right_device, options);

    RtcHeadlessSession rtc_session(options);
    if (!rtc_session.Init()) {
      return 1;
    }

    bool first_frame_logged = false;

    while (!StopRequested()) {
      rtc_session.Tick();

      UyvyV4l2CaptureDevice::CapturedFrame left_frame;
      if (!left_device.DequeueCapturedFrame(&left_frame)) {
        continue;
      }
      CapturedFrameGuard left_guard(&left_device, &left_frame);

      UyvyV4l2CaptureDevice::CapturedFrame right_frame;
      if (!right_device.DequeueCapturedFrame(&right_frame)) {
        continue;
      }
      CapturedFrameGuard right_guard(&right_device, &right_frame);

      rtc_session.NoteCapturedFrame();
      if (!first_frame_logged) {
        first_frame_logged = true;
        LogInfo("first stitched frame captured");
      }

      if (!rtc_session.IsReadyToSend()) {
        continue;
      }

      const uint8_t* stitched_i420 = nullptr;
      size_t stitched_i420_size = 0;
      if (!converter.Convert(left_frame.data, left_frame.bytes_used,
                             right_frame.data, right_frame.bytes_used,
                             &stitched_i420, &stitched_i420_size,
                             &cuda_error)) {
        throw std::runtime_error(
            "failed to convert and stitch dual UYVY frames on CUDA: " +
            cuda_error);
      }

      if (!rtc_session.SendI420Frame(
              stitched_i420, stitched_i420_size, stitched_width,
              stitched_height, converter.y_stride(), converter.u_stride(),
              converter.v_stride())) {
        throw std::runtime_error("failed to send stitched I420 frame");
      }

      if (options.frame_limit > 0 &&
          rtc_session.sent_frames() >=
              static_cast<uint64_t>(options.frame_limit)) {
        LogInfo("frame limit reached");
        RequestStop();
        break;
      }
    }

    return 0;
  } catch (const std::exception& ex) {
    LogError(std::string("rtc_dual_camera_headless failed: ") + ex.what());
    return 1;
  }
}
