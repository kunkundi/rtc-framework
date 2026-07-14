#include "rtc_dual_camera/dual_camera_async_image_source.h"
#include "rtc_dual_camera/dual_uyvy_frame_converter.h"
#include "rtc_headless/rtc_camera_common.h"
#include "rtc_headless/rtc_headless_session.h"
#include "rtc_vision/yolo_frame_consumer.h"

#include <stddef.h>
#include <stdint.h>

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

using namespace rtc_camera_headless;

namespace {

struct DualCaptureOptions {
  CaptureOptions rtc_options;
  rtc_dual_camera::AsyncDualCameraImageSourceOptions image_options;
  YoloFrameConsumerOptions yolo_options;
};

size_t ClampYoloDownscaleFromConfig(size_t value) {
  if (value < 1) {
    return 1;
  }
  if (value > 8) {
    return 8;
  }
  return value;
}

bool TryReadDownscale(const nlohmann::json& object,
                      const char* key,
                      size_t* downscale) {
  if (!object.is_object() || !object.contains(key) || !downscale) {
    return false;
  }

  const nlohmann::json& value = object.at(key);
  if (value.is_number_unsigned()) {
    *downscale = ClampYoloDownscaleFromConfig(value.get<size_t>());
    return true;
  }
  if (value.is_number_integer()) {
    const int parsed = value.get<int>();
    *downscale =
        ClampYoloDownscaleFromConfig(parsed > 0 ? static_cast<size_t>(parsed)
                                                : 1);
    return true;
  }
  return false;
}

YoloFrameConsumerOptions LoadYoloOptionsFromConfig(
    const std::string& config_path) {
  YoloFrameConsumerOptions options;
  if (config_path.empty() || !FileExists(config_path)) {
    return options;
  }

  std::ifstream input(config_path);
  if (!input.good()) {
    return options;
  }

  nlohmann::json root;
  input >> root;

  size_t downscale = options.processing_downscale;
  if (root.contains("vision_processing") &&
      TryReadDownscale(root.at("vision_processing"), "downscale",
                       &downscale)) {
    options.processing_downscale = downscale;
  } else if (TryReadDownscale(root, "yolo_opencv_downscale", &downscale)) {
    options.processing_downscale = downscale;
  }

  return options;
}

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
      << "publishes raw camera pairs, and lets consumers convert them for RTC\n"
      << "or vision workloads.\n"
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
      options.image_options.left_device = require_value("--left-device");
      continue;
    }
    if (arg == "--right-device") {
      options.image_options.right_device = require_value("--right-device");
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

  options.image_options.width = options.rtc_options.width;
  options.image_options.height = options.rtc_options.height;
  options.image_options.buffer_count = options.rtc_options.buffer_count;
  options.image_options.timeout_ms = options.rtc_options.timeout_ms;
  options.image_options.warmup_frames = options.rtc_options.warmup_frames;
  options.image_options.warmup_delay_ms =
      options.rtc_options.warmup_delay_ms;
  options.yolo_options = LoadYoloOptionsFromConfig(
      ResolveConfigPath(options.rtc_options.config_path));

  return options;
}

}  // namespace

int main(int argc, char** argv) {
  InstallSignalHandlers();

  try {
    const DualCaptureOptions dual_options = ParseDualArgs(argc, argv);
    const CaptureOptions& options = dual_options.rtc_options;

    rtc_dual_camera::AsyncDualCameraImageSource video_source(
        dual_options.image_options);
    std::shared_ptr<rtc_dual_camera::AsyncImageFrameSubscription> rtc_frames =
        video_source.Subscribe(2);
    std::shared_ptr<rtc_dual_camera::AsyncImageFrameSubscription> yolo_frames =
        video_source.Subscribe(1);
    video_source.Start();

    YoloFrameConsumer yolo_consumer(yolo_frames, dual_options.yolo_options);
    yolo_consumer.Start();
    DualUyvyFrameConverter rtc_frame_converter;

    RtcHeadlessSession rtc_session(options);
    if (!rtc_session.Init()) {
      return 1;
    }

    bool first_frame_logged = false;

    while (!StopRequested()) {
      rtc_session.Tick();

      rtc_dual_camera::ImageFrame frame;
      if (!rtc_frames->WaitNext(&frame, std::chrono::milliseconds(50))) {
        if (video_source.failed()) {
          throw std::runtime_error("video source failed: " +
                                   video_source.error_message());
        }
        continue;
      }

      rtc_session.NoteCapturedFrame();
      if (!first_frame_logged) {
        first_frame_logged = true;
        LogInfo("first raw frame received by RTC consumer");
      }

      if (!rtc_session.IsReadyToSend()) {
        continue;
      }

      if (frame.empty()) {
        throw std::runtime_error("video source returned an invalid frame");
      }

      ConvertedI420Frame converted_frame;
      std::string convert_error;
      if (!rtc_frame_converter.ConvertToI420(frame, &converted_frame,
                                             &convert_error)) {
        throw std::runtime_error("failed to convert RTC frame to I420: " +
                                 convert_error);
      }

      if (!rtc_session.SendI420Frame(
              converted_frame.data, converted_frame.data_size,
              converted_frame.width, converted_frame.height,
              converted_frame.stride_y, converted_frame.stride_u,
              converted_frame.stride_v)) {
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
