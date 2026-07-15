#include "edge_application.h"

#include "rtc_edge/camera_video_sources.h"
#include "rtc_headless/rtc_camera_common.h"
#include "rtc_headless/rtc_headless_session.h"
#include "rtc_vehicle/vehicle_control_interface.h"
#include "rtc_vehicle_protocol/vehicle_control_protocol.h"

#include <stdint.h>

#include <chrono>
#include <stdexcept>
#include <string>
#include <vector>

namespace rtc_edge_headless {
namespace {

using rtc_camera_headless::LogError;
using rtc_camera_headless::LogInfo;
using rtc_camera_headless::RtcHeadlessSession;

RtcHeadlessSession::DataChannelConfig MakeDataChannel(
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

RtcHeadlessSession::ExternalVideoSourceConfig MakeVideoSource(
    const char* source_id) {
  RtcHeadlessSession::ExternalVideoSourceConfig source;
  source.source_id = source_id;
  source.priority = RtcPriorityType::High;
  return source;
}

RtcHeadlessSession::Features MakeVehicleRtcFeatures() {
  RtcHeadlessSession::Features features;
  features.enable_data_channel = false;
  features.enable_external_video_source = true;
  features.external_video_source_id = rtc_edge::kStereoCameraVideoSourceId;
  features.room_action = RtcHeadlessSession::RoomAction::Join;
  features.additional_data_channels.push_back(MakeDataChannel(
      vts_rtc::vehicle::kVehicleControlChannelLabel, RtcPriorityType::High,
      false, 0));
  features.additional_data_channels.push_back(MakeDataChannel(
      vts_rtc::vehicle::kVehicleEventChannelLabel, RtcPriorityType::High,
      true, -1));
  features.additional_data_channels.push_back(MakeDataChannel(
      vts_rtc::vehicle::kVehicleStateChannelLabel, RtcPriorityType::Medium,
      false, 0));
  features.additional_external_video_sources.push_back(
      MakeVideoSource(rtc_edge::kSurroundFrontVideoSourceId));
  features.additional_external_video_sources.push_back(
      MakeVideoSource(rtc_edge::kSurroundRearVideoSourceId));
  features.additional_external_video_sources.push_back(
      MakeVideoSource(rtc_edge::kSurroundLeftVideoSourceId));
  features.additional_external_video_sources.push_back(
      MakeVideoSource(rtc_edge::kSurroundRightVideoSourceId));
  return features;
}

uint64_t GetSteadyTimeMs() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

bool SendControlData(RtcSessionId remote_sessionid,
                     const char* label,
                     const std::vector<uint8_t>& payload) {
  if (label == nullptr || payload.empty()) {
    return false;
  }
  return RtcSendData(remote_sessionid, label,
                     reinterpret_cast<const char*>(payload.data()),
                     payload.size()) == RtcErrorCode::OK;
}

void WriteInfoLog(const std::string& message) {
  LogInfo(message);
}

void WriteErrorLog(const std::string& message) {
  LogError(message);
}

void HandleReceivedMessage(
    rtc_vehicle::VehicleControlModule* control_module,
    RtcSessionId remote_sessionid,
    RtcDataChannelLabel label,
    const char* message,
    size_t message_size) {
  if (control_module == nullptr) {
    return;
  }
  control_module->EnqueueMessage(remote_sessionid, label, message,
                                 message_size);
}

void HandleP2PState(rtc_vehicle::VehicleControlModule* control_module,
                    RtcSessionId remote_sessionid,
                    RtcP2PState state) {
  if (control_module == nullptr) {
    return;
  }
  control_module->EnqueueP2PState(remote_sessionid, state);
}

void HandleDataChannelState(
    rtc_vehicle::VehicleControlModule* control_module,
    RtcSessionId remote_sessionid,
    RtcDataChannelLabel label,
    RtcDataChannelState state) {
  if (control_module == nullptr || label == nullptr ||
      state != DataChannelClosed) {
    return;
  }

  const std::string channel(label);
  const bool is_control_channel =
      channel == vts_rtc::vehicle::kVehicleControlChannelLabel;
  const bool is_event_channel =
      channel == vts_rtc::vehicle::kVehicleEventChannelLabel;
  if (is_control_channel || is_event_channel) {
    control_module->EnqueueP2PState(remote_sessionid, P2PClosed);
  }
}

void HandleServerConnectionState(
    rtc_vehicle::VehicleControlModule* control_module,
    RtcServerConnectionState state) {
  if (control_module != nullptr && state != ServerLogined) {
    control_module->EnqueueTransportDisconnected();
  }
}

RtcHeadlessSession::Callbacks MakeVehicleRtcCallbacks(
    rtc_vehicle::VehicleControlModule* control_module) {
  RtcHeadlessSession::Callbacks callbacks;

  // RTC 回调需要记住控制模块指针，这里的 lambda 只负责转发参数。
  callbacks.recv_message =
      [control_module](RtcSessionId remote_sessionid,
                       RtcDataChannelLabel label, const char* message,
                       size_t message_size) {
        HandleReceivedMessage(control_module, remote_sessionid, label, message,
                              message_size);
      };
  callbacks.p2p_state =
      [control_module](RtcSessionId remote_sessionid, RtcP2PState state) {
        HandleP2PState(control_module, remote_sessionid, state);
      };
  callbacks.datachannel_state =
      [control_module](RtcSessionId remote_sessionid,
                       RtcDataChannelLabel label,
                       RtcDataChannelState state) {
        HandleDataChannelState(control_module, remote_sessionid, label, state);
      };
  callbacks.server_connection_state =
      [control_module](RtcServerConnectionState state) {
        HandleServerConnectionState(control_module, state);
      };
  return callbacks;
}

void ShutdownApplication(
    rtc_vehicle::VehicleControlModule& control_module,
    RtcHeadlessSession& rtc_session,
    rtc_edge::DualCameraStreamingModule& camera_module) {
  control_module.Shutdown();
  rtc_session.Shutdown();
  camera_module.Stop();
}

void RunMainLoop(const EdgeOptions& options,
                 rtc_vehicle::VehicleControlModule& control_module,
                 RtcHeadlessSession& rtc_session,
                 rtc_edge::DualCameraStreamingModule& camera_module) {
  while (!rtc_camera_headless::StopRequested()) {
    rtc_session.Tick();
    control_module.Tick(GetSteadyTimeMs());

    std::string camera_error;
    if (!camera_module.Tick(&rtc_session, &camera_error)) {
      throw std::runtime_error(camera_error);
    }

    if (options.rtc.frame_limit > 0) {
      const uint64_t frame_limit =
          static_cast<uint64_t>(options.rtc.frame_limit);
      if (rtc_session.sent_frames() >= frame_limit) {
        LogInfo("已达到视频帧发送上限");
        rtc_camera_headless::RequestStop();
      }
    }
  }
}

}  // 匿名命名空间

int RunEdgeApplication(const EdgeOptions& options) {
  rtc_vehicle::PlaceholderVehicleControlInterface vehicle_control(
      &WriteInfoLog);
  rtc_vehicle::VehicleControlModule control_module(
      &vehicle_control, &SendControlData, &WriteInfoLog, &WriteErrorLog,
      options.control);

  const RtcHeadlessSession::Callbacks callbacks =
      MakeVehicleRtcCallbacks(&control_module);
  RtcHeadlessSession rtc_session(options.rtc, MakeVehicleRtcFeatures(),
                                 callbacks);
  rtc_edge::DualCameraStreamingModule camera_module(options.camera);

  try {
    std::string control_error;
    if (!control_module.Start(&control_error)) {
      throw std::runtime_error(std::string("控制模块启动失败：") +
                               control_error);
    }
    if (!rtc_session.Init()) {
      throw std::runtime_error("RTC 会话初始化失败");
    }

    std::string camera_error;
    if (!camera_module.Start(&camera_error)) {
      throw std::runtime_error(std::string("摄像头模块启动失败：") +
                               camera_error);
    }

    RunMainLoop(options, control_module, rtc_session, camera_module);
  } catch (...) {
    // 运行中任意步骤失败时，都按相同顺序关闭已启动的模块。
    ShutdownApplication(control_module, rtc_session, camera_module);
    throw;
  }

  ShutdownApplication(control_module, rtc_session, camera_module);
  return 0;
}

}  // 命名空间 rtc_edge_headless
