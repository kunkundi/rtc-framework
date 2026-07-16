#include "edge_application.h"

#include "rtc_edge/camera_video_sources.h"
#include "rtc_edge/single_camera_streaming_module.h"
#include "rtc_logging/rtc_logging.h"
#include "rtc_runtime/process_runtime.h"
#include "rtc_runtime/rtc_session.h"
#include "rtc_vehicle/vehicle_control_interface.h"
#include "rtc_vehicle_protocol/vehicle_control_protocol.h"
#include "rtc_vision/vision_detection_codec.h"

#include <stdint.h>

#include <chrono>
#include <stdexcept>
#include <string>
#include <vector>

namespace rtc_edge_app {
namespace {

using rtc_runtime::RtcSession;

rtc_edge::SingleCameraStreamingModuleOptions MakeSurroundCameraOptions(
    const SurroundCameraOptions& surround,
    const std::string& device,
    const char* video_source_id,
    const char* camera_name) {
  rtc_edge::SingleCameraStreamingModuleOptions options;
  options.capture.device = device;
  options.capture.width = surround.width;
  options.capture.height = surround.height;
  options.capture.buffer_count = surround.buffer_count;
  options.capture.timeout_ms = surround.timeout_ms;
  options.capture.warmup_frames = surround.warmup_frames;
  options.capture.warmup_delay_ms = surround.warmup_delay_ms;
  options.video_source_id = video_source_id;
  options.camera_name = camera_name;
  options.frame_wait = surround.frame_wait;
  return options;
}

class EdgeCameraModules {
 public:
  explicit EdgeCameraModules(const EdgeOptions& options)
      : front_enabled_(!options.surround_camera.front_device.empty()),
        rear_enabled_(!options.surround_camera.rear_device.empty()),
        left_enabled_(!options.surround_camera.left_device.empty()),
        right_enabled_(!options.surround_camera.right_device.empty()),
        stereo_camera_(options.camera),
        front_camera_(MakeSurroundCameraOptions(
            options.surround_camera, options.surround_camera.front_device,
            rtc_edge::kSurroundFrontVideoSourceId, "surround front camera")),
        rear_camera_(MakeSurroundCameraOptions(
            options.surround_camera, options.surround_camera.rear_device,
            rtc_edge::kSurroundRearVideoSourceId, "surround rear camera")),
        left_camera_(MakeSurroundCameraOptions(
            options.surround_camera, options.surround_camera.left_device,
            rtc_edge::kSurroundLeftVideoSourceId, "surround left camera")),
        right_camera_(MakeSurroundCameraOptions(
            options.surround_camera, options.surround_camera.right_device,
            rtc_edge::kSurroundRightVideoSourceId,
            "surround right camera")) {}

  bool Start(std::string* error_message) {
    std::string module_error;
    if (!stereo_camera_.Start(&module_error)) {
      SetStartError("stereo camera", module_error, error_message);
      Stop();
      return false;
    }
    if (front_enabled_ && !front_camera_.Start(&module_error)) {
      SetStartError("surround front camera", module_error, error_message);
      Stop();
      return false;
    }
    if (rear_enabled_ && !rear_camera_.Start(&module_error)) {
      SetStartError("surround rear camera", module_error, error_message);
      Stop();
      return false;
    }
    if (left_enabled_ && !left_camera_.Start(&module_error)) {
      SetStartError("surround left camera", module_error, error_message);
      Stop();
      return false;
    }
    if (right_enabled_ && !right_camera_.Start(&module_error)) {
      SetStartError("surround right camera", module_error, error_message);
      Stop();
      return false;
    }
    return true;
  }

  void Stop() {
    if (right_enabled_) {
      right_camera_.RequestStop();
    }
    if (left_enabled_) {
      left_camera_.RequestStop();
    }
    if (rear_enabled_) {
      rear_camera_.RequestStop();
    }
    if (front_enabled_) {
      front_camera_.RequestStop();
    }
    stereo_camera_.Stop();
    if (right_enabled_) {
      right_camera_.Stop();
    }
    if (left_enabled_) {
      left_camera_.Stop();
    }
    if (rear_enabled_) {
      rear_camera_.Stop();
    }
    if (front_enabled_) {
      front_camera_.Stop();
    }
  }

  bool Tick(RtcSession* rtc_session, std::string* error_message) {
    if (!stereo_camera_.Tick(rtc_session, error_message)) {
      return false;
    }
    if (front_enabled_ && !front_camera_.Tick(rtc_session, error_message)) {
      return false;
    }
    if (rear_enabled_ && !rear_camera_.Tick(rtc_session, error_message)) {
      return false;
    }
    if (left_enabled_ && !left_camera_.Tick(rtc_session, error_message)) {
      return false;
    }
    if (right_enabled_ && !right_camera_.Tick(rtc_session, error_message)) {
      return false;
    }
    return true;
  }

 private:
  void SetStartError(const char* module_name,
                     const std::string& module_error,
                     std::string* error_message) {
    if (error_message != nullptr) {
      *error_message = std::string(module_name) + " failed to start: " +
                       module_error;
    }
  }

  bool front_enabled_ = false;
  bool rear_enabled_ = false;
  bool left_enabled_ = false;
  bool right_enabled_ = false;
  rtc_edge::DualCameraStreamingModule stereo_camera_;
  rtc_edge::SingleCameraStreamingModule front_camera_;
  rtc_edge::SingleCameraStreamingModule rear_camera_;
  rtc_edge::SingleCameraStreamingModule left_camera_;
  rtc_edge::SingleCameraStreamingModule right_camera_;
};

RtcSession::DataChannelConfig MakeDataChannel(
    const char* label,
    RtcPriorityType priority,
    bool ordered,
    int max_retransmits) {
  RtcSession::DataChannelConfig channel;
  channel.label = label;
  channel.priority = priority;
  channel.ordered = ordered;
  channel.max_retransmits = max_retransmits;
  return channel;
}

RtcSession::ExternalVideoSourceConfig MakeVideoSource(
    const char* source_id) {
  RtcSession::ExternalVideoSourceConfig source;
  source.source_id = source_id;
  source.priority = RtcPriorityType::High;
  return source;
}

RtcSession::Features MakeVehicleRtcFeatures(
    const SurroundCameraOptions& surround,
    bool yolo_enabled) {
  RtcSession::Features features;
  features.enable_data_channel = false;
  features.enable_external_video_source = true;
  features.external_video_source_id = rtc_edge::kStereoCameraVideoSourceId;
  features.room_action = RtcSession::RoomAction::Join;
  features.additional_data_channels.push_back(MakeDataChannel(
      vts_rtc::vehicle::kVehicleControlChannelLabel, RtcPriorityType::High,
      false, 0));
  features.additional_data_channels.push_back(MakeDataChannel(
      vts_rtc::vehicle::kVehicleEventChannelLabel, RtcPriorityType::High,
      true, -1));
  features.additional_data_channels.push_back(MakeDataChannel(
      vts_rtc::vehicle::kVehicleStateChannelLabel, RtcPriorityType::Medium,
      false, 0));
  if (yolo_enabled) {
    features.additional_data_channels.push_back(MakeDataChannel(
        vts_rtc::vision::kVisionDetectionChannelLabel,
        RtcPriorityType::Medium, false, 0));
  }
  if (!surround.front_device.empty()) {
    features.additional_external_video_sources.push_back(
        MakeVideoSource(rtc_edge::kSurroundFrontVideoSourceId));
  }
  if (!surround.rear_device.empty()) {
    features.additional_external_video_sources.push_back(
        MakeVideoSource(rtc_edge::kSurroundRearVideoSourceId));
  }
  if (!surround.left_device.empty()) {
    features.additional_external_video_sources.push_back(
        MakeVideoSource(rtc_edge::kSurroundLeftVideoSourceId));
  }
  if (!surround.right_device.empty()) {
    features.additional_external_video_sources.push_back(
        MakeVideoSource(rtc_edge::kSurroundRightVideoSourceId));
  }
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
  rtc_logging::LogInfo(message);
}

void WriteErrorLog(const std::string& message) {
  rtc_logging::LogError(message);
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

RtcSession::Callbacks MakeVehicleRtcCallbacks(
    rtc_vehicle::VehicleControlModule* control_module) {
  RtcSession::Callbacks callbacks;

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
    RtcSession& rtc_session,
    EdgeCameraModules& camera_modules) {
  control_module.Shutdown();
  camera_modules.Stop();
  rtc_session.Shutdown();
}

void RunMainLoop(const EdgeOptions& options,
                 rtc_vehicle::VehicleControlModule& control_module,
                 RtcSession& rtc_session,
                 EdgeCameraModules& camera_modules) {
  while (!rtc_runtime::StopRequested()) {
    rtc_session.Tick();
    control_module.Tick(GetSteadyTimeMs());

    std::string camera_error;
    if (!camera_modules.Tick(&rtc_session, &camera_error)) {
      throw std::runtime_error(camera_error);
    }

    if (options.frame_limit > 0) {
      const uint64_t frame_limit =
          static_cast<uint64_t>(options.frame_limit);
      if (rtc_session.sent_frames() >= frame_limit) {
        rtc_logging::LogInfo("Video frame send limit reached");
        rtc_runtime::RequestStop();
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

  const RtcSession::Callbacks callbacks =
      MakeVehicleRtcCallbacks(&control_module);
  RtcSession rtc_session(
      options.rtc,
      MakeVehicleRtcFeatures(options.surround_camera,
                             options.camera.yolo_enabled),
      callbacks);
  EdgeCameraModules camera_modules(options);

  try {
    std::string control_error;
    if (!control_module.Start(&control_error)) {
      throw std::runtime_error(std::string("Vehicle control module failed to start: ") +
                               control_error);
    }
    if (!rtc_session.Init()) {
      throw std::runtime_error("RTC session initialization failed");
    }

    std::string camera_error;
    if (!camera_modules.Start(&camera_error)) {
      throw std::runtime_error(std::string("Camera modules failed to start: ") +
                               camera_error);
    }

    RunMainLoop(options, control_module, rtc_session, camera_modules);
  } catch (...) {
    // 运行中任意步骤失败时，都按相同顺序关闭已启动的模块。
    ShutdownApplication(control_module, rtc_session, camera_modules);
    throw;
  }

  ShutdownApplication(control_module, rtc_session, camera_modules);
  return 0;
}

}  // 命名空间 rtc_edge_app
