#pragma once

#include "c_rtc.h"
#include "rtc_vehicle/vehicle_control_interface.h"

#include <stddef.h>
#include <stdint.h>

#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace rtc_vehicle {

struct VehicleControlModuleOptions {
  uint32_t watchdog_ms = vts_rtc::vehicle::kDefaultDriveWatchdogMs;
  uint32_t state_interval_ms = 50;
  size_t max_pending_events = 128;
};

class VehicleControlModule {
 public:
  using SendDataCallback = std::function<bool(
      RtcSessionId, const char*, const std::vector<uint8_t>&)>;
  using LogCallback = std::function<void(const std::string&)>;

  VehicleControlModule(VehicleControlInterface* vehicle_control,
                       const SendDataCallback& send_data,
                       const LogCallback& log_info,
                       const LogCallback& log_error,
                       const VehicleControlModuleOptions& options =
                           VehicleControlModuleOptions());
  ~VehicleControlModule();

  VehicleControlModule(const VehicleControlModule&) = delete;
  VehicleControlModule& operator=(const VehicleControlModule&) = delete;

  bool Start(std::string* error_message);
  void Shutdown();
  void EnqueueMessage(RtcSessionId remote_sessionid,
                      RtcDataChannelLabel label,
                      const char* message,
                      size_t message_size);
  void EnqueueP2PState(RtcSessionId remote_sessionid, RtcP2PState state);
  void EnqueueTransportDisconnected();
  void Tick(uint64_t now_ms);

  bool started() const { return started_; }
  bool has_active_peer() const { return has_active_session_; }
  RtcSessionId active_sessionid() const { return active_sessionid_; }
  vts_rtc::vehicle::VehicleGear active_gear() const { return active_gear_; }

 private:
  enum class PendingEventType {
    Message,
    PeerConnected,
    PeerDisconnected,
    TransportDisconnected,
  };

  struct PendingEvent {
    PendingEventType type = PendingEventType::Message;
    RtcSessionId remote_sessionid = 0;
    std::string label;
    std::vector<uint8_t> payload;
    bool payload_invalid = false;
  };

  void Enqueue(PendingEvent event);
  void ProcessEvent(const PendingEvent& event, uint64_t now_ms);
  void ProcessMessage(const PendingEvent& event, uint64_t now_ms);
  void ProcessDriveCommand(const vts_rtc::vehicle::Envelope& envelope,
                           uint64_t now_ms);
  void ProcessSetGear(RtcSessionId remote_sessionid,
                      const vts_rtc::vehicle::Envelope& envelope);
  void HandlePeerConnected(RtcSessionId remote_sessionid);
  void HandlePeerDisconnected(RtcSessionId remote_sessionid);
  void StopForSafety(const std::string& reason);
  void SendEventAck(RtcSessionId remote_sessionid,
                    const vts_rtc::vehicle::SetGear& request,
                    const VehicleCommandResult& result);
  void MaybeSendState(uint64_t now_ms);
  bool SendPayload(RtcSessionId remote_sessionid,
                   const char* label,
                   const vts_rtc::vehicle::EncodeResult& encoded);

  VehicleControlInterface* vehicle_control_ = nullptr;
  SendDataCallback send_data_;
  LogCallback log_info_;
  LogCallback log_error_;
  VehicleControlModuleOptions options_;
  vts_rtc::vehicle::DriveCommandGate drive_gate_;
  bool started_ = false;
  bool has_active_session_ = false;
  bool stop_sent_ = true;
  bool awaiting_first_drive_ = false;
  bool safety_latched_ = true;
  bool state_dirty_ = false;
  RtcSessionId active_sessionid_ = 0;
  vts_rtc::vehicle::VehicleGear active_gear_ =
      vts_rtc::vehicle::VehicleGear::Neutral;
  uint64_t outgoing_seq_ = 1;
  uint64_t last_state_sent_ms_ = 0;
  std::mutex pending_mutex_;
  std::deque<PendingEvent> pending_events_;
  bool pending_overflow_ = false;
};

}  // 命名空间 rtc_vehicle
