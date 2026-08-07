#include "edge_application.h"

#include "video_send_planner.h"

#include "rtc_edge/camera_video_sources.h"
#include "rtc_edge/single_camera_streaming_module.h"
#include "rtc_logging/rtc_logging.h"
#include "rtc_runtime/process_runtime.h"
#include "rtc_runtime/rtc_session.h"
#include "rtc_vehicle/vehicle_control_interface.h"
#include "rtc_vehicle_protocol/vehicle_control_protocol.h"
#include "rtc_vision/vision_detection_codec.h"
#include "rtc_vision/view_control_protocol.h"
#include "rtc_dog/dog_command_forwarder.h"

#include <stdint.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace rtc_edge_app {
namespace {

using rtc_runtime::RtcSession;

constexpr uint64_t kInactiveVideoViewSendIntervalMs = 1000;
constexpr uint64_t kMainLoopSleepMs = 1;

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
        video_send_planner_(std::chrono::milliseconds(
            kInactiveVideoViewSendIntervalMs)),
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

  ~EdgeCameraModules() {
    Stop();
  }

  bool Start(RtcSession* rtc_session, std::string* error_message) {
    if (started_) {
      return true;
    }
    if (rtc_session == nullptr) {
      if (error_message != nullptr) {
        *error_message = "RTC session must not be null";
      }
      return false;
    }

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

    {
      std::lock_guard<std::mutex> lock(worker_mutex_);
      rtc_session_ = rtc_session;
      stop_requested_ = false;
      worker_error_.clear();
      ResetWorkerState(&stereo_worker_);
      ResetWorkerState(&front_worker_);
      ResetWorkerState(&rear_worker_);
      ResetWorkerState(&left_worker_);
      ResetWorkerState(&right_worker_);
    }

    try {
      stereo_worker_.thread = std::thread([this] {
        RunCameraWorker(&stereo_worker_, &stereo_camera_);
      });
      if (front_enabled_) {
        front_worker_.thread = std::thread([this] {
          RunCameraWorker(&front_worker_, &front_camera_);
        });
      }
      if (rear_enabled_) {
        rear_worker_.thread = std::thread([this] {
          RunCameraWorker(&rear_worker_, &rear_camera_);
        });
      }
      if (left_enabled_) {
        left_worker_.thread = std::thread([this] {
          RunCameraWorker(&left_worker_, &left_camera_);
        });
      }
      if (right_enabled_) {
        right_worker_.thread = std::thread([this] {
          RunCameraWorker(&right_worker_, &right_camera_);
        });
      }
    } catch (const std::exception& ex) {
      SetStartError("camera streaming worker", ex.what(), error_message);
      Stop();
      return false;
    }

    started_ = true;
    return true;
  }

  void Stop() {
    {
      std::lock_guard<std::mutex> lock(worker_mutex_);
      stop_requested_ = true;
    }
    worker_condition_.notify_all();

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
    stereo_camera_.RequestStop();

    JoinWorker(&right_worker_);
    JoinWorker(&left_worker_);
    JoinWorker(&rear_worker_);
    JoinWorker(&front_worker_);
    JoinWorker(&stereo_worker_);

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
    rtc_session_ = nullptr;
    started_ = false;
  }

  bool Tick(std::string* error_message) {
    const VideoSendPlan send_plan = CurrentVideoSendPlan();
    bool notify_workers = false;
    {
      std::lock_guard<std::mutex> lock(worker_mutex_);
      if (!worker_error_.empty()) {
        if (error_message != nullptr) {
          *error_message = worker_error_;
        }
        return false;
      }
      if (stop_requested_) {
        return true;
      }

      notify_workers |= UpdateWorkerPlan(send_plan.stereo, &stereo_worker_);
      if (front_enabled_) {
        notify_workers |= UpdateWorkerPlan(send_plan.front, &front_worker_);
      }
      if (rear_enabled_) {
        notify_workers |= UpdateWorkerPlan(send_plan.rear, &rear_worker_);
      }
      if (left_enabled_) {
        notify_workers |= UpdateWorkerPlan(send_plan.left, &left_worker_);
      }
      if (right_enabled_) {
        notify_workers |= UpdateWorkerPlan(send_plan.right, &right_worker_);
      }
    }
    if (notify_workers) {
      worker_condition_.notify_all();
    }
    return true;
  }

  void SetEnabledView(vts_rtc::vision::ViewMode enabled_view) {
    if (enabled_view != vts_rtc::vision::ViewMode::Binocular &&
        enabled_view != vts_rtc::vision::ViewMode::Surround) {
      return;
    }
    std::lock_guard<std::mutex> lock(video_view_mutex_);
    enabled_view_ = enabled_view;
  }

 private:
  struct CameraWorkerState {
    bool continuous = false;
    bool pending_send = false;
    std::thread thread;
  };

  static void ResetWorkerState(CameraWorkerState* worker) {
    if (worker == nullptr) {
      return;
    }
    worker->continuous = false;
    worker->pending_send = false;
  }

  static bool UpdateWorkerPlan(const CameraSendPlan& plan,
                               CameraWorkerState* worker) {
    if (worker == nullptr) {
      return false;
    }
    bool changed = worker->continuous != plan.continuous;
    worker->continuous = plan.continuous;
    if (worker->continuous) {
      if (worker->pending_send) {
        worker->pending_send = false;
        changed = true;
      }
    } else if (plan.send_once && !worker->pending_send) {
      worker->pending_send = true;
      changed = true;
    }
    return changed;
  }

  VideoSendPlan CurrentVideoSendPlan() {
    std::lock_guard<std::mutex> lock(video_view_mutex_);
    const bool binocular_enabled =
        enabled_view_ == vts_rtc::vision::ViewMode::Binocular;
    return video_send_planner_.Next(binocular_enabled,
                                    std::chrono::steady_clock::now());
  }

  bool WaitForWorker(CameraWorkerState* worker, bool* continuous) {
    std::unique_lock<std::mutex> lock(worker_mutex_);
    worker_condition_.wait(lock, [this, worker] {
      return stop_requested_ || worker->continuous || worker->pending_send;
    });
    if (stop_requested_) {
      return false;
    }
    if (continuous != nullptr) {
      *continuous = worker->continuous;
    }
    worker->pending_send = false;
    return true;
  }

  template <typename Module>
  void RunCameraWorker(CameraWorkerState* worker, Module* module) {
    bool continuous = false;
    while (WaitForWorker(worker, &continuous)) {
      std::string module_error;
      if (!module->Tick(rtc_session_, true, continuous, &module_error)) {
        ReportWorkerError(module_error);
        return;
      }
    }
  }

  void ReportWorkerError(const std::string& error_message) {
    {
      std::lock_guard<std::mutex> lock(worker_mutex_);
      if (worker_error_.empty()) {
        worker_error_ = error_message.empty()
                            ? "Camera streaming worker failed"
                            : error_message;
      }
      stop_requested_ = true;
    }
    worker_condition_.notify_all();
  }

  static void JoinWorker(CameraWorkerState* worker) {
    if (worker != nullptr && worker->thread.joinable()) {
      worker->thread.join();
    }
  }

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
  VideoSendPlanner video_send_planner_;
  rtc_edge::DualCameraStreamingModule stereo_camera_;
  rtc_edge::SingleCameraStreamingModule front_camera_;
  rtc_edge::SingleCameraStreamingModule rear_camera_;
  rtc_edge::SingleCameraStreamingModule left_camera_;
  rtc_edge::SingleCameraStreamingModule right_camera_;
  RtcSession* rtc_session_ = nullptr;
  bool started_ = false;
  std::mutex worker_mutex_;
  std::condition_variable worker_condition_;
  bool stop_requested_ = true;
  std::string worker_error_;
  CameraWorkerState stereo_worker_;
  CameraWorkerState front_worker_;
  CameraWorkerState rear_worker_;
  CameraWorkerState left_worker_;
  CameraWorkerState right_worker_;
  std::mutex video_view_mutex_;
  vts_rtc::vision::ViewMode enabled_view_ =
      vts_rtc::vision::ViewMode::Binocular;
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
  features.additional_data_channels.push_back(MakeDataChannel(
      vts_rtc::vision::kVideoViewControlChannelLabel, RtcPriorityType::High,
      true, -1));
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
    EdgeCameraModules* camera_modules,
    RtcSessionId remote_sessionid,
    RtcDataChannelLabel label,
    const char* message,
    size_t message_size) {
  if (label != nullptr &&
      std::string(label) == vts_rtc::vision::kVideoViewControlChannelLabel) {
    if (camera_modules == nullptr) {
      return;
    }
    const vts_rtc::vision::ViewControlDecodeResult decoded =
        vts_rtc::vision::DecodeViewControl(
            reinterpret_cast<const uint8_t*>(message), message_size);
    if (!decoded) {
      rtc_logging::LogError(std::string("Video view control decode failed: ") +
                            decoded.error_message);
      return;
    }
    camera_modules->SetEnabledView(decoded.envelope.command.enable_view);
    rtc_logging::LogInfo(
        std::string("Video view enabled: ") +
        vts_rtc::vision::ViewModeText(decoded.envelope.command.enable_view));
    return;
  }
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
    rtc_vehicle::VehicleControlModule* control_module,
    EdgeCameraModules* camera_modules) {
  RtcSession::Callbacks callbacks;

  // RTC 回调需要记住控制模块指针，这里的 lambda 只负责转发参数。
  callbacks.recv_message =
      [control_module, camera_modules](RtcSessionId remote_sessionid,
                                       RtcDataChannelLabel label,
                                       const char* message,
                                       size_t message_size) {
        HandleReceivedMessage(control_module, camera_modules, remote_sessionid,
                              label, message, message_size);
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
    if (!camera_modules.Tick(&camera_error)) {
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
    std::this_thread::sleep_for(
        std::chrono::milliseconds(kMainLoopSleepMs));
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

  EdgeCameraModules camera_modules(options);
  const RtcSession::Callbacks callbacks =
      MakeVehicleRtcCallbacks(&control_module, &camera_modules);
  RtcSession rtc_session(
      options.rtc,
      MakeVehicleRtcFeatures(options.surround_camera,
                             options.camera.yolo_enabled),
      callbacks);

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
    if (!camera_modules.Start(&rtc_session, &camera_error)) {
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
