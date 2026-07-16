#include "rtc_camera/dual/async_image_source.h"
#include "rtc_camera/dual/frame_converter.h"
#include "rtc_logging/rtc_logging.h"
#include "rtc_runtime/process_runtime.h"
#include "rtc_runtime/rtc_session.h"
#include "rtc_vision/vision_detection_codec.h"
#include "rtc_vision/yolo_frame_consumer.h"

#include <stddef.h>
#include <stdint.h>

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

using namespace rtc_camera_headless;

namespace {

struct DualCaptureOptions {
  rtc_runtime::SessionOptions rtc_options;
  rtc_camera::dual::AsyncDualCameraImageSourceOptions image_options;
  YoloFrameConsumerOptions yolo_options;
  int frame_limit = 0;
};

bool ParseNonNegativeInt(const std::string& text, int* value) {
  char* end = nullptr;
  const long parsed = std::strtol(text.c_str(), &end, 10);
  if (end == nullptr || end == text.c_str() || *end != '\0' || parsed < 0 ||
      parsed > std::numeric_limits<int>::max()) {
    return false;
  }
  *value = static_cast<int>(parsed);
  return true;
}

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
  if (config_path.empty() || !rtc_runtime::FileExists(config_path)) {
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

rtc_runtime::RtcSession::Features MakeRtcFeatures() {
  rtc_runtime::RtcSession::Features features;
  features.enable_data_channel = true;
  features.enable_external_video_source = true;
  features.external_video_source_id = "merged_image";
  rtc_runtime::RtcSession::DataChannelConfig vision_channel;
  vision_channel.label = vts_rtc::vision::kVisionDetectionChannelLabel;
  vision_channel.priority = RtcPriorityType::Medium;
  vision_channel.ordered = false;
  vision_channel.max_retransmits = 0;
  features.additional_data_channels.push_back(vision_channel);
  return features;
}

DualCaptureOptions ParseDualArgs(int argc, char** argv) {
  DualCaptureOptions options;

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
    if (arg == "--device") {
      throw std::runtime_error(
          "--device is not supported in the dual-camera client, use "
          "--left-device/--right-device instead");
    }
    if (arg == "--room") {
      options.rtc_options.room_id = require_value("--room");
      continue;
    }
    if (arg == "--config") {
      options.rtc_options.config_path = require_value("--config");
      continue;
    }

    int* value = nullptr;
    int minimum = 0;
    if (arg == "--width") {
      value = &options.image_options.width;
    } else if (arg == "--height") {
      value = &options.image_options.height;
    } else if (arg == "--frame-limit") {
      value = &options.frame_limit;
    } else if (arg == "--buffer-count") {
      value = &options.image_options.buffer_count;
      minimum = 2;
    } else if (arg == "--timeout-ms") {
      value = &options.image_options.timeout_ms;
    } else if (arg == "--warmup-frames") {
      value = &options.image_options.warmup_frames;
    } else if (arg == "--warmup-delay-ms") {
      value = &options.image_options.warmup_delay_ms;
    } else if (arg == "--join-retry-ms") {
      value = &options.rtc_options.join_retry_ms;
    } else if (arg == "--status-interval-sec") {
      value = &options.rtc_options.status_interval_sec;
    } else {
      throw std::runtime_error("unknown argument: " + arg);
    }

    if (!ParseNonNegativeInt(require_value(arg.c_str()), value) ||
        *value < minimum) {
      throw std::runtime_error("invalid " + arg + " value");
    }
  }

  options.yolo_options = LoadYoloOptionsFromConfig(
      rtc_runtime::ResolveConfigPath(options.rtc_options.config_path));

  return options;
}

}  // namespace

int main(int argc, char** argv) {
  rtc_runtime::InstallSignalHandlers();

  try {
    const DualCaptureOptions dual_options = ParseDualArgs(argc, argv);
    const rtc_runtime::SessionOptions& options = dual_options.rtc_options;

    rtc_camera::dual::AsyncDualCameraImageSource video_source(
        dual_options.image_options);
    std::shared_ptr<rtc_camera::dual::AsyncImageFrameSubscription> rtc_frames =
        video_source.Subscribe(2);
    std::shared_ptr<rtc_camera::dual::AsyncImageFrameSubscription> yolo_frames =
        video_source.Subscribe(1);
    video_source.Start();

    YoloFrameConsumer yolo_consumer(yolo_frames, dual_options.yolo_options);
    yolo_consumer.Start();
    rtc_camera::dual::DualUyvyFrameConverter rtc_frame_converter;

    rtc_runtime::RtcSession rtc_session(options, MakeRtcFeatures());
    if (!rtc_session.Init()) {
      return 1;
    }

    bool first_frame_logged = false;

    while (!rtc_runtime::StopRequested()) {
      rtc_session.Tick();

      rtc_camera::dual::ImageFrame frame;
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
        rtc_logging::LogInfo("first raw frame received by RTC consumer");
      }

      if (!rtc_session.IsReadyToSend()) {
        continue;
      }

      if (frame.empty()) {
        throw std::runtime_error("video source returned an invalid frame");
      }

      rtc_camera::dual::ConvertedI420Frame converted_frame;
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

      if (dual_options.frame_limit > 0 &&
          rtc_session.sent_frames() >=
              static_cast<uint64_t>(dual_options.frame_limit)) {
        rtc_logging::LogInfo("frame limit reached");
        rtc_runtime::RequestStop();
        break;
      }
    }

    return 0;
  } catch (const std::exception& ex) {
    rtc_logging::LogError(std::string("rtc_dual_camera_headless failed: ") + ex.what());
    return 1;
  }
}
