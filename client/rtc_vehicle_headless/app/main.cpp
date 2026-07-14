#include "industrial_control_interface.h"
#include "rtc_camera_common.h"
#include "rtc_control/vehicle_control_protocol.h"
#include "rtc_headless_session.h"
#include "vehicle_camera_module.h"
#include "vehicle_control_module.h"

#include <stdint.h>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using rtc_camera_headless::CaptureOptions;
using rtc_camera_headless::LogError;
using rtc_camera_headless::LogInfo;
using rtc_camera_headless::RtcHeadlessSession;

struct VehicleMainOptions {
  CaptureOptions rtc;
  rtc_vehicle::VehicleCameraModuleOptions camera;
  rtc_vehicle::VehicleControlModuleOptions control;
};

bool CommonOptionTakesValue(const std::string& argument) {
  return argument == "--device" || argument == "--room" ||
         argument == "--config" || argument == "--width" ||
         argument == "--height" || argument == "--frame-limit" ||
         argument == "--buffer-count" || argument == "--timeout-ms" ||
         argument == "--warmup-frames" ||
         argument == "--warmup-delay-ms" ||
         argument == "--join-retry-ms" ||
         argument == "--status-interval-sec";
}

uint32_t ParsePositiveUint32(const std::string& text, const char* name) {
  char* end = nullptr;
  const unsigned long parsed = std::strtoul(text.c_str(), &end, 10);
  if (!end || *end != '\0' || parsed == 0 ||
      parsed > std::numeric_limits<uint32_t>::max()) {
    throw std::runtime_error(std::string("参数无效：") + name);
  }
  return static_cast<uint32_t>(parsed);
}

void PrintUsage(const char* program) {
  std::cout
      << "用法：" << program << " [选项]\n\n"
      << "车辆端主程序：采集双目摄像头、发送 RTC 视频，并接收车辆控制协议。\n\n"
      << "车辆选项：\n"
      << "  --left-device /dev/video0        左摄像头设备\n"
      << "  --right-device /dev/video1       右摄像头设备\n"
      << "  --control-watchdog-ms 300         控制看门狗时间\n"
      << "  --control-state-interval-ms 50    控制状态发送周期\n\n"
      << "RTC 与采集选项：\n"
      << "  --room zhejianglab                自动加入的房间\n"
      << "  --config rtc.cfg                  RTC 配置文件\n"
      << "  --width 1280                      单路采集宽度\n"
      << "  --height 720                      单路采集高度\n"
      << "  --buffer-count 4                  摄像头 mmap 缓冲区数量\n"
      << "  --timeout-ms 2000                 摄像头等待超时\n"
      << "  --frame-limit 0                   发送指定帧数后退出，0 表示不限制\n"
      << "  --help                            显示帮助\n"
      << std::endl;
}

VehicleMainOptions ParseVehicleArgs(int argc, char** argv) {
  VehicleMainOptions options;
  std::vector<char*> forwarded;
  forwarded.reserve(static_cast<size_t>(argc));
  forwarded.push_back(argv[0]);

  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    auto require_value = [&](const char* name) -> std::string {
      if (i + 1 >= argc) {
        throw std::runtime_error(std::string("参数缺少取值：") + name);
      }
      return argv[++i];
    };

    if (argument == "--help" || argument == "-h") {
      PrintUsage(argv[0]);
      std::exit(0);
    }
    if (argument == "--left-device") {
      options.camera.capture.left_device = require_value("--left-device");
      continue;
    }
    if (argument == "--right-device") {
      options.camera.capture.right_device = require_value("--right-device");
      continue;
    }
    if (argument == "--control-watchdog-ms") {
      options.control.watchdog_ms = ParsePositiveUint32(
          require_value("--control-watchdog-ms"), "--control-watchdog-ms");
      if (options.control.watchdog_ms >
          vts_rtc::control::kDefaultDriveWatchdogMs) {
        throw std::runtime_error("控制看门狗不能超过 300 ms");
      }
      continue;
    }
    if (argument == "--control-state-interval-ms") {
      options.control.state_interval_ms = ParsePositiveUint32(
          require_value("--control-state-interval-ms"),
          "--control-state-interval-ms");
      continue;
    }

    forwarded.push_back(argv[i]);
    if (CommonOptionTakesValue(argument)) {
      if (i + 1 >= argc) {
        throw std::runtime_error(std::string("参数缺少取值：") + argument);
      }
      forwarded.push_back(argv[++i]);
    }
  }

  options.rtc = rtc_camera_headless::ParseArgs(
      static_cast<int>(forwarded.size()), forwarded.data());
  if (!options.rtc.device.empty()) {
    throw std::runtime_error(
        "车辆主程序不支持 --device，请使用 --left-device 和 --right-device");
  }
  options.camera.capture.width = options.rtc.width;
  options.camera.capture.height = options.rtc.height;
  options.camera.capture.buffer_count = options.rtc.buffer_count;
  options.camera.capture.timeout_ms = options.rtc.timeout_ms;
  options.camera.capture.warmup_frames = options.rtc.warmup_frames;
  options.camera.capture.warmup_delay_ms = options.rtc.warmup_delay_ms;
  return options;
}

RtcHeadlessSession::DataChannelConfig MakeChannel(
    const char* label,
    RtcPriorityType priority,
    bool ordered,
    int max_retransmits) {
  RtcHeadlessSession::DataChannelConfig channel;
  channel.label = label;
  channel.priority = priority;
  channel.ordered = ordered;
  channel.max_retransmits = max_retransmits;
  return channel;
}

RtcHeadlessSession::Features MakeFeatures() {
  RtcHeadlessSession::Features features;
  features.enable_data_channel = false;
  features.enable_external_video_source = true;
  features.room_action = RtcHeadlessSession::RoomAction::Join;
  features.additional_data_channels.push_back(MakeChannel(
      vts_rtc::control::kVehicleControlChannelLabel, RtcPriorityType::High,
      false, 0));
  features.additional_data_channels.push_back(MakeChannel(
      vts_rtc::control::kVehicleEventChannelLabel, RtcPriorityType::High,
      true, -1));
  features.additional_data_channels.push_back(MakeChannel(
      vts_rtc::control::kVehicleStateChannelLabel, RtcPriorityType::Medium,
      false, 0));
  return features;
}

uint64_t SteadyNowMs() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

bool SendControlData(RtcSessionId remote_sessionid,
                     const char* label,
                     const std::vector<uint8_t>& payload) {
  if (!label || payload.empty()) {
    return false;
  }
  return RtcSendData(remote_sessionid, label,
                     reinterpret_cast<const char*>(payload.data()),
                     payload.size()) == RtcErrorCode::OK;
}

}  // 匿名命名空间

int main(int argc, char** argv) {
  rtc_camera_headless::InstallSignalHandlers();

  try {
    const VehicleMainOptions options = ParseVehicleArgs(argc, argv);

    rtc_vehicle::PlaceholderIndustrialControlInterface industrial_control(
        [](const std::string& message) { LogInfo(message); });
    rtc_vehicle::VehicleControlModule control_module(
        &industrial_control, &SendControlData,
        [](const std::string& message) { LogInfo(message); },
        [](const std::string& message) { LogError(message); }, options.control);

    std::string control_error;
    if (!control_module.Start(&control_error)) {
      throw std::runtime_error(std::string("控制模块启动失败：") +
                               control_error);
    }

    RtcHeadlessSession::Callbacks callbacks;
    callbacks.recv_message =
        [&control_module](RtcSessionId remote_sessionid,
                          RtcDataChannelLabel label, const char* message,
                          size_t message_size) {
          control_module.EnqueueMessage(remote_sessionid, label, message,
                                        message_size);
        };
    callbacks.p2p_state =
        [&control_module](RtcSessionId remote_sessionid, RtcP2PState state) {
          control_module.EnqueueP2PState(remote_sessionid, state);
        };
    callbacks.datachannel_state =
        [&control_module](RtcSessionId remote_sessionid,
                          RtcDataChannelLabel label,
                          RtcDataChannelState state) {
          if (state != DataChannelClosed || !label) {
            return;
          }
          const std::string channel(label);
          if (channel == vts_rtc::control::kVehicleControlChannelLabel ||
              channel == vts_rtc::control::kVehicleEventChannelLabel) {
            control_module.EnqueueP2PState(remote_sessionid, P2PClosed);
          }
        };
    callbacks.server_connection_state =
        [&control_module](RtcServerConnectionState state) {
          if (state != ServerLogined) {
            control_module.EnqueueTransportDisconnected();
          }
        };

    RtcHeadlessSession rtc_session(options.rtc, MakeFeatures(), callbacks);
    if (!rtc_session.Init()) {
      throw std::runtime_error("RTC 会话初始化失败");
    }

    rtc_vehicle::VehicleCameraModule camera_module(options.camera);
    try {
      std::string camera_error;
      if (!camera_module.Start(&camera_error)) {
        throw std::runtime_error(std::string("摄像头模块启动失败：") +
                                 camera_error);
      }

      while (!rtc_camera_headless::StopRequested()) {
        rtc_session.Tick();
        control_module.Tick(SteadyNowMs());

        camera_error.clear();
        if (!camera_module.Tick(&rtc_session, &camera_error)) {
          throw std::runtime_error(camera_error);
        }

        if (options.rtc.frame_limit > 0 &&
            rtc_session.sent_frames() >=
                static_cast<uint64_t>(options.rtc.frame_limit)) {
          LogInfo("已达到视频帧发送上限");
          rtc_camera_headless::RequestStop();
        }
      }
    } catch (...) {
      control_module.Shutdown();
      rtc_session.Shutdown();
      camera_module.Stop();
      throw;
    }

    control_module.Shutdown();
    rtc_session.Shutdown();
    camera_module.Stop();
    return 0;
  } catch (const std::exception& ex) {
    LogError(std::string("rtc_vehicle_headless 运行失败：") + ex.what());
    return 1;
  }
}
