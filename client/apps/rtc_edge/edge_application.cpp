#include "edge_application.h"

#include "rtc_edge/camera_video_sources.h"
<<<<<<< HEAD
=======
#include "rtc_edge/simulated_surround_streaming_module.h"
>>>>>>> 5c8f59e (新加双目和环路切换)
#include "rtc_edge/single_camera_streaming_module.h"
#include "rtc_logging/rtc_logging.h"
#include "rtc_runtime/process_runtime.h"
#include "rtc_runtime/rtc_session.h"
#include "rtc_vehicle/vehicle_control_interface.h"
#include "rtc_vehicle_protocol/vehicle_control_protocol.h"
#include "rtc_vision/vision_detection_codec.h"
#include "rtc_dog/dog_command_forwarder.h"

#include <stdint.h>

<<<<<<< HEAD
=======
#include <atomic>
>>>>>>> 5c8f59e (新加双目和环路切换)
#include <chrono>
#include <stdexcept>
#include <string>
#include <vector>

namespace rtc_edge_app {
namespace {

using rtc_runtime::RtcSession;

<<<<<<< HEAD
=======
constexpr const char* kVideoViewControlChannelLabel = "video.view_control.v1";
constexpr int kStereoKeepaliveMs = 500;

>>>>>>> 5c8f59e (新加双目和环路切换)
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

<<<<<<< HEAD
class EdgeCameraModules {
 public:
  explicit EdgeCameraModules(const EdgeOptions& options)
      : front_enabled_(!options.surround_camera.front_device.empty()),
=======
rtc_edge::SimulatedSurroundStreamingModuleOptions
MakeSimulatedSurroundCameraOptions(const SurroundCameraOptions& surround) {
  rtc_edge::SimulatedSurroundStreamingModuleOptions options;
  options.yuv_path = surround.simulation_yuv_path;
  options.width = static_cast<size_t>(surround.width);
  options.height = static_cast<size_t>(surround.height);
  options.fps = surround.simulation_fps;
  return options;
}

class EdgeCameraModules {
 public:
  explicit EdgeCameraModules(const EdgeOptions& options)
      : simulation_enabled_(options.surround_camera.simulate),
        front_enabled_(!options.surround_camera.front_device.empty()),
>>>>>>> 5c8f59e (新加双目和环路切换)
        rear_enabled_(!options.surround_camera.rear_device.empty()),
        left_enabled_(!options.surround_camera.left_device.empty()),
        right_enabled_(!options.surround_camera.right_device.empty()),
        stereo_camera_(options.camera),
<<<<<<< HEAD
=======
        simulated_surround_(MakeSimulatedSurroundCameraOptions(
            options.surround_camera)),
>>>>>>> 5c8f59e (新加双目和环路切换)
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
<<<<<<< HEAD
=======
    if (simulation_enabled_) {
      if (!simulated_surround_.Start(&module_error)) {
        SetStartError("simulated surround camera", module_error,
                      error_message);
        Stop();
        return false;
      }
      return true;
    }
>>>>>>> 5c8f59e (新加双目和环路切换)
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
<<<<<<< HEAD
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
=======
    if (!simulation_enabled_ && right_enabled_) {
      right_camera_.RequestStop();
    }
    if (!simulation_enabled_ && left_enabled_) {
      left_camera_.RequestStop();
    }
    if (!simulation_enabled_ && rear_enabled_) {
      rear_camera_.RequestStop();
    }
    if (!simulation_enabled_ && front_enabled_) {
      front_camera_.RequestStop();
    }
    stereo_camera_.Stop();
    simulated_surround_.Stop();
    if (!simulation_enabled_ && right_enabled_) {
      right_camera_.Stop();
    }
    if (!simulation_enabled_ && left_enabled_) {
      left_camera_.Stop();
    }
    if (!simulation_enabled_ && rear_enabled_) {
      rear_camera_.Stop();
    }
    if (!simulation_enabled_ && front_enabled_) {
>>>>>>> 5c8f59e (新加双目和环路切换)
      front_camera_.Stop();
    }
  }

  bool Tick(RtcSession* rtc_session, std::string* error_message) {
<<<<<<< HEAD
=======
    if (!simulation_enabled_) {
      return TickPhysicalCameras(rtc_session, error_message);
    }

    const bool surround_view = surround_view_.load();
    const bool send_stereo =
        !surround_view || ShouldSendStereoKeepalive();
    if (!stereo_camera_.Tick(rtc_session, error_message, send_stereo,
                             !surround_view)) {
      return false;
    }
    if (surround_view &&
        !simulated_surround_.Tick(rtc_session, error_message)) {
      return false;
    }
    return true;
  }

  void SetSurroundView(bool enabled) {
    if (enabled && !simulation_enabled_) {
      rtc_logging::LogError(
          "Surround view request ignored because simulation is disabled");
      return;
    }

    const bool previous = surround_view_.exchange(enabled);
    if (previous != enabled) {
      rtc_logging::LogInfo(enabled ? "Video view changed: stereo -> surround"
                                   : "Video view changed: surround -> stereo");
    }
  }

 private:
  bool TickPhysicalCameras(RtcSession* rtc_session,
                           std::string* error_message) {
>>>>>>> 5c8f59e (新加双目和环路切换)
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

<<<<<<< HEAD
 private:
=======
  bool ShouldSendStereoKeepalive() {
    const std::chrono::steady_clock::time_point now =
        std::chrono::steady_clock::now();
    if (last_stereo_keepalive_time_.time_since_epoch().count() != 0 &&
        now - last_stereo_keepalive_time_ <
            std::chrono::milliseconds(kStereoKeepaliveMs)) {
      return false;
    }
    last_stereo_keepalive_time_ = now;
    return true;
  }

>>>>>>> 5c8f59e (新加双目和环路切换)
  void SetStartError(const char* module_name,
                     const std::string& module_error,
                     std::string* error_message) {
    if (error_message != nullptr) {
      *error_message = std::string(module_name) + " failed to start: " +
                       module_error;
    }
  }

<<<<<<< HEAD
=======
  const bool simulation_enabled_ = false;
  std::atomic<bool> surround_view_{false};
  std::chrono::steady_clock::time_point last_stereo_keepalive_time_{};
>>>>>>> 5c8f59e (新加双目和环路切换)
  bool front_enabled_ = false;
  bool rear_enabled_ = false;
  bool left_enabled_ = false;
  bool right_enabled_ = false;
  rtc_edge::DualCameraStreamingModule stereo_camera_;
<<<<<<< HEAD
=======
  rtc_edge::SimulatedSurroundStreamingModule simulated_surround_;
>>>>>>> 5c8f59e (新加双目和环路切换)
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
<<<<<<< HEAD
=======
  features.additional_data_channels.push_back(MakeDataChannel(
      kVideoViewControlChannelLabel, RtcPriorityType::High, true, -1));
>>>>>>> 5c8f59e (新加双目和环路切换)
  if (yolo_enabled) {
    features.additional_data_channels.push_back(MakeDataChannel(
        vts_rtc::vision::kVisionDetectionChannelLabel,
        RtcPriorityType::Medium, false, 0));
  }
<<<<<<< HEAD
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
=======
  if (surround.simulate || !surround.front_device.empty()) {
    features.additional_external_video_sources.push_back(
        MakeVideoSource(rtc_edge::kSurroundFrontVideoSourceId));
  }
  if (surround.simulate || !surround.rear_device.empty()) {
    features.additional_external_video_sources.push_back(
        MakeVideoSource(rtc_edge::kSurroundRearVideoSourceId));
  }
  if (surround.simulate || !surround.left_device.empty()) {
    features.additional_external_video_sources.push_back(
        MakeVideoSource(rtc_edge::kSurroundLeftVideoSourceId));
  }
  if (surround.simulate || !surround.right_device.empty()) {
>>>>>>> 5c8f59e (新加双目和环路切换)
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
<<<<<<< HEAD
=======
    EdgeCameraModules* camera_modules,
>>>>>>> 5c8f59e (新加双目和环路切换)
    RtcSessionId remote_sessionid,
    RtcDataChannelLabel label,
    const char* message,
    size_t message_size) {
<<<<<<< HEAD
=======
  if (label != nullptr &&
      std::string(label) == kVideoViewControlChannelLabel) {
    if (camera_modules == nullptr ||
        (message == nullptr && message_size != 0)) {
      return;
    }

    const std::string payload(message == nullptr ? "" : message,
                              message_size);
    if (payload == "mode=stereo") {
      camera_modules->SetSurroundView(false);
    } else if (payload == "mode=surround") {
      camera_modules->SetSurroundView(true);
    } else {
      rtc_logging::LogError("Unknown video view control message: " + payload);
    }
    return;
  }

>>>>>>> 5c8f59e (新加双目和环路切换)
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
<<<<<<< HEAD
    rtc_vehicle::VehicleControlModule* control_module) {
=======
    rtc_vehicle::VehicleControlModule* control_module,
    EdgeCameraModules* camera_modules) {
>>>>>>> 5c8f59e (新加双目和环路切换)
  RtcSession::Callbacks callbacks;

  // RTC 回调需要记住控制模块指针，这里的 lambda 只负责转发参数。
  callbacks.recv_message =
<<<<<<< HEAD
      [control_module](RtcSessionId remote_sessionid,
                       RtcDataChannelLabel label, const char* message,
                       size_t message_size) {
        HandleReceivedMessage(control_module, remote_sessionid, label, message,
                              message_size);
=======
      [control_module, camera_modules](RtcSessionId remote_sessionid,
                                       RtcDataChannelLabel label,
                                       const char* message,
                                       size_t message_size) {
        HandleReceivedMessage(control_module, camera_modules,
                              remote_sessionid, label, message, message_size);
>>>>>>> 5c8f59e (新加双目和环路切换)
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
  // 根据配置选择 VehicleControlInterface 实现：
  //   - 启用狗控制 → DogCommandForwarder（DriveCommand → rosbridge → 狗）
  //   - 否则       → PlaceholderVehicleControlInterface（仅日志）
  std::unique_ptr<rtc_dog::DogCommandForwarder> dog_forwarder;
  std::unique_ptr<rtc_vehicle::PlaceholderVehicleControlInterface>
      placeholder_control;
  rtc_vehicle::VehicleControlInterface* vehicle_interface = nullptr;

  rtc_vehicle::VehicleControlModuleOptions control_options = options.control;
  if (options.dog_control.enabled) {
    rtc_dog::DogCommandForwarder::Config dog_config;
    dog_config.rosbridge_url = options.dog_control.rosbridge_url;
    dog_config.reconnect_interval_ms =
        options.dog_control.reconnect_interval_ms;
    dog_config.max_forward_speed = options.dog_control.max_forward_speed;
    dog_config.max_angular_speed = options.dog_control.max_angular_speed;
    dog_forwarder =
        std::make_unique<rtc_dog::DogCommandForwarder>(dog_config);
    vehicle_interface = dog_forwarder.get();
    // 狗模式：可恢复故障保持停车，连接恢复后仅接受更大的新序号。
    control_options.allow_watchdog_recovery = true;
    control_options.allow_interface_recovery = true;
  } else {
    placeholder_control =
        std::make_unique<rtc_vehicle::PlaceholderVehicleControlInterface>(
            &WriteInfoLog);
    vehicle_interface = placeholder_control.get();
  }

  rtc_vehicle::VehicleControlModule control_module(
      vehicle_interface, &SendControlData, &WriteInfoLog, &WriteErrorLog,
      control_options);

<<<<<<< HEAD
  const RtcSession::Callbacks callbacks =
      MakeVehicleRtcCallbacks(&control_module);
=======
  EdgeCameraModules camera_modules(options);
  const RtcSession::Callbacks callbacks =
      MakeVehicleRtcCallbacks(&control_module, &camera_modules);
>>>>>>> 5c8f59e (新加双目和环路切换)
  RtcSession rtc_session(
      options.rtc,
      MakeVehicleRtcFeatures(options.surround_camera,
                             options.camera.yolo_enabled),
      callbacks);
<<<<<<< HEAD
  EdgeCameraModules camera_modules(options);
=======
>>>>>>> 5c8f59e (新加双目和环路切换)

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
