#include "rtc_vehicle/vehicle_control_module.h"

#include <utility>

namespace rtc_vehicle {
namespace {

constexpr size_t kMaxControlPayloadSize = 4096;

bool IsDisconnectedState(RtcP2PState state) {
  return state == P2PDisconnected || state == P2PFailed || state == P2PClosed;
}

VehicleCommandResult NormalizeResult(
    const VehicleCommandResult& source) {
  VehicleCommandResult result = source;
  if (result.accepted) {
    result.error_code = vts_rtc::vehicle::VehicleErrorCode::None;
  } else if (result.error_code == vts_rtc::vehicle::VehicleErrorCode::None) {
    result.error_code = vts_rtc::vehicle::VehicleErrorCode::Internal;
  }
  return result;
}

}  // 匿名命名空间

VehicleControlModule::VehicleControlModule(
    VehicleControlInterface* vehicle_control,
    const SendDataCallback& send_data,
    const LogCallback& log_info,
    const LogCallback& log_error,
    const VehicleControlModuleOptions& options)
    : vehicle_control_(vehicle_control),
      send_data_(send_data),
      log_info_(log_info),
      log_error_(log_error),
      options_(options),
      drive_gate_(options.watchdog_ms) {
  if (options_.state_interval_ms == 0) {
    options_.state_interval_ms = 50;
  }
  if (options_.max_pending_events == 0) {
    options_.max_pending_events = 128;
  }
}

VehicleControlModule::~VehicleControlModule() {
  Shutdown();
}

bool VehicleControlModule::Start(std::string* error_message) {
  if (started_) {
    return true;
  }
  if (!vehicle_control_) {
    if (error_message) {
      *error_message = "vehicle control interface is null";
    }
    return false;
  }
  if (options_.watchdog_ms >
      vts_rtc::vehicle::kDefaultDriveWatchdogMs) {
    if (error_message) {
      *error_message = "control watchdog exceeds the protocol limit of 300 ms";
    }
    return false;
  }
  if (!vehicle_control_->Open(error_message)) {
    return false;
  }
  started_ = true;
  return true;
}

void VehicleControlModule::Shutdown() {
  if (!started_) {
    return;
  }
  StopForSafety("control module shutdown");
  vehicle_control_->Close();
  started_ = false;
  has_active_session_ = false;
  active_sessionid_ = 0;
  std::lock_guard<std::mutex> lock(pending_mutex_);
  pending_events_.clear();
  pending_overflow_ = false;
}

void VehicleControlModule::EnqueueMessage(RtcSessionId remote_sessionid,
                                           RtcDataChannelLabel label,
                                           const char* message,
                                           size_t message_size) {
  if (!label) {
    return;
  }
  const std::string channel(label);
  if (channel != vts_rtc::vehicle::kVehicleControlChannelLabel &&
      channel != vts_rtc::vehicle::kVehicleEventChannelLabel) {
    return;
  }

  PendingEvent event;
  event.type = PendingEventType::Message;
  event.remote_sessionid = remote_sessionid;
  event.label = channel;
  if (!message || message_size == 0 ||
      message_size > kMaxControlPayloadSize) {
    event.payload_invalid = true;
  } else {
    const uint8_t* begin = reinterpret_cast<const uint8_t*>(message);
    event.payload.assign(begin, begin + message_size);
  }
  Enqueue(std::move(event));
}

void VehicleControlModule::EnqueueP2PState(RtcSessionId remote_sessionid,
                                           RtcP2PState state) {
  PendingEvent event;
  event.remote_sessionid = remote_sessionid;
  if (state == P2PConnected) {
    event.type = PendingEventType::PeerConnected;
  } else if (IsDisconnectedState(state)) {
    event.type = PendingEventType::PeerDisconnected;
  } else {
    return;
  }
  Enqueue(std::move(event));
}

void VehicleControlModule::EnqueueTransportDisconnected() {
  PendingEvent event;
  event.type = PendingEventType::TransportDisconnected;
  Enqueue(std::move(event));
}

void VehicleControlModule::Tick(uint64_t now_ms) {
  if (!started_) {
    return;
  }

  std::deque<PendingEvent> events;
  bool overflowed = false;
  {
    std::lock_guard<std::mutex> lock(pending_mutex_);
    events.swap(pending_events_);
    overflowed = pending_overflow_;
    pending_overflow_ = false;
  }

  if (overflowed) {
    StopForSafety("control event queue overflow");
  }
  for (const PendingEvent& event : events) {
    ProcessEvent(event, now_ms);
  }

  if (has_active_session_ && drive_gate_.started() &&
      drive_gate_.PollWatchdog(now_ms)) {
    if (options_.allow_watchdog_recovery) {
      if (!stop_sent_ && vehicle_control_) {
        vehicle_control_->SendStop();
        stop_sent_ = true;
      }
      drive_gate_.Stop();
      awaiting_first_drive_ = true;
      safety_latched_ = false;
      state_dirty_ = true;
      if (log_info_) {
        log_info_("Watchdog recovery stop: drive command watchdog timeout");
      }
    } else {
      StopForSafety("drive command watchdog timeout");
    }
  }
  MaybeSendState(now_ms);
}

void VehicleControlModule::Enqueue(PendingEvent event) {
  std::lock_guard<std::mutex> lock(pending_mutex_);
  if (pending_events_.size() >= options_.max_pending_events) {
    pending_events_.clear();
    pending_overflow_ = true;
  }
  pending_events_.push_back(std::move(event));
}

void VehicleControlModule::ProcessEvent(const PendingEvent& event,
                                        uint64_t now_ms) {
  switch (event.type) {
    case PendingEventType::Message:
      ProcessMessage(event, now_ms);
      break;
    case PendingEventType::PeerConnected:
      HandlePeerConnected(event.remote_sessionid);
      break;
    case PendingEventType::PeerDisconnected:
      HandlePeerDisconnected(event.remote_sessionid);
      break;
    case PendingEventType::TransportDisconnected:
      if (has_active_session_) {
        HandlePeerDisconnected(active_sessionid_);
      }
      break;
  }
}

void VehicleControlModule::ProcessMessage(const PendingEvent& event,
                                          uint64_t now_ms) {
  if (!has_active_session_ || event.remote_sessionid != active_sessionid_) {
    if (log_error_) {
      log_error_("Ignoring control message from inactive peer");
    }
    return;
  }
  if (event.payload_invalid) {
    StopForSafety("control message is empty, has null data, or exceeds 4 KiB");
    return;
  }

  const vts_rtc::vehicle::DecodeResult decoded =
      vts_rtc::vehicle::DecodeEnvelope(event.payload);
  if (!decoded) {
    StopForSafety(std::string("control protocol decode failed: ") +
                  decoded.error_message);
    return;
  }

  if (event.label == vts_rtc::vehicle::kVehicleControlChannelLabel) {
    if (decoded.envelope.type != vts_rtc::vehicle::MessageType::DriveCommand) {
      StopForSafety("non-drive message received on realtime control channel");
      return;
    }
    ProcessDriveCommand(decoded.envelope, now_ms);
    return;
  }

  if (decoded.envelope.type != vts_rtc::vehicle::MessageType::SetGear) {
    StopForSafety(
        "non-gear transaction received on reliable control channel");
    return;
  }
  ProcessSetGear(event.remote_sessionid, decoded.envelope);
}

void VehicleControlModule::ProcessDriveCommand(
    const vts_rtc::vehicle::Envelope& envelope,
    uint64_t now_ms) {
  if (awaiting_first_drive_) {
    drive_gate_.Start(now_ms);
  }
  const vts_rtc::vehicle::DriveReceiveResult gate_result =
      drive_gate_.Accept(envelope.seq, envelope.drive_command, now_ms);
  if (gate_result.status ==
      vts_rtc::vehicle::DriveReceiveStatus::DuplicateOrOutOfOrder) {
    return;
  }
  if (gate_result.status != vts_rtc::vehicle::DriveReceiveStatus::Accepted) {
    if (gate_result.should_stop) {
      if (options_.allow_watchdog_recovery) {
        // 狗模式：仅发送停车指令，不锁死，等待下一条合法指令恢复。
        if (!stop_sent_ && vehicle_control_) {
          vehicle_control_->SendStop();
          stop_sent_ = true;
        }
        drive_gate_.Stop();
        awaiting_first_drive_ = true;
        safety_latched_ = false;
        state_dirty_ = true;
        if (log_info_) {
          log_info_(std::string("Watchdog recovery stop: ") +
                    gate_result.error_message);
        }
      } else {
        StopForSafety(gate_result.error_message);
      }
    }
    return;
  }
  awaiting_first_drive_ = false;

  const VehicleCommandResult result = NormalizeResult(
      vehicle_control_->SendDriveCommand(envelope.drive_command));
  if (!result.accepted) {
    StopForSafety(result.detail.empty() ? "local vehicle control interface rejected drive command"
                                        : result.detail);
    return;
  }
  stop_sent_ = false;
  state_dirty_ = true;
}

void VehicleControlModule::ProcessSetGear(
    RtcSessionId remote_sessionid,
    const vts_rtc::vehicle::Envelope& envelope) {
  if (safety_latched_) {
    VehicleCommandResult rejected;
    rejected.accepted = false;
    rejected.error_code = vts_rtc::vehicle::VehicleErrorCode::InvalidState;
    rejected.detail = "vehicle is safety-locked";
    SendEventAck(remote_sessionid, envelope.set_gear, rejected);
    state_dirty_ = true;
    return;
  }
  const VehicleCommandResult result = NormalizeResult(
      vehicle_control_->SendGearCommand(envelope.set_gear.gear));
  if (result.accepted) {
    active_gear_ = envelope.set_gear.gear;
  }
  SendEventAck(remote_sessionid, envelope.set_gear, result);
  state_dirty_ = true;
}

void VehicleControlModule::HandlePeerConnected(RtcSessionId remote_sessionid) {
  if (has_active_session_) {
    if (active_sessionid_ != remote_sessionid && log_error_) {
      log_error_(
          "Ignoring additional P2P session because a control peer is active");
    }
    return;
  }
  has_active_session_ = true;
  active_sessionid_ = remote_sessionid;
  drive_gate_.Stop();
  awaiting_first_drive_ = true;
  safety_latched_ = false;
  stop_sent_ = false;
  state_dirty_ = true;
  if (log_info_) {
    log_info_("Vehicle control peer connected");
  }
}

void VehicleControlModule::HandlePeerDisconnected(
    RtcSessionId remote_sessionid) {
  if (!has_active_session_ || active_sessionid_ != remote_sessionid) {
    return;
  }
  StopForSafety("vehicle control peer disconnected");
  has_active_session_ = false;
  active_sessionid_ = 0;
  state_dirty_ = false;
}

void VehicleControlModule::StopForSafety(const std::string& reason) {
  const bool first_stop = drive_gate_.started() || !stop_sent_;
  drive_gate_.Stop();
  awaiting_first_drive_ = false;
  safety_latched_ = true;
  if (!stop_sent_ && vehicle_control_) {
    vehicle_control_->SendStop();
    stop_sent_ = true;
  }
  state_dirty_ = true;
  if (first_stop && !reason.empty() && log_error_) {
    log_error_(std::string("Safety stop triggered: ") + reason);
  }
}

void VehicleControlModule::SendEventAck(
    RtcSessionId remote_sessionid,
    const vts_rtc::vehicle::SetGear& request,
    const VehicleCommandResult& source_result) {
  const VehicleCommandResult result = NormalizeResult(source_result);
  vts_rtc::vehicle::EventAck ack;
  ack.request_id = request.request_id;
  ack.accepted = result.accepted;
  ack.error_code = result.error_code;
  ack.active_gear = active_gear_;
  SendPayload(remote_sessionid, vts_rtc::vehicle::kVehicleEventChannelLabel,
              vts_rtc::vehicle::EncodeEventAck(outgoing_seq_++, ack));
}

void VehicleControlModule::MaybeSendState(uint64_t now_ms) {
  if (!has_active_session_) {
    return;
  }
  if (!state_dirty_ && last_state_sent_ms_ != 0 &&
      now_ms - last_state_sent_ms_ < options_.state_interval_ms) {
    return;
  }

  vts_rtc::vehicle::VehicleState state;
  state.active_gear = active_gear_;
  state.last_received_drive_seq = drive_gate_.last_received_seq();
  state.watchdog_stopped = safety_latched_;
  if (SendPayload(active_sessionid_,
                  vts_rtc::vehicle::kVehicleStateChannelLabel,
                  vts_rtc::vehicle::EncodeVehicleState(outgoing_seq_++,
                                                       state))) {
    last_state_sent_ms_ = now_ms;
    state_dirty_ = false;
  }
}

bool VehicleControlModule::SendPayload(
    RtcSessionId remote_sessionid,
    const char* label,
    const vts_rtc::vehicle::EncodeResult& encoded) {
  if (!encoded) {
    if (log_error_) {
      log_error_(std::string("Control protocol encode failed: ") +
                 encoded.error_message);
    }
    return false;
  }
  if (!send_data_ || !send_data_(remote_sessionid, label, encoded.payload)) {
    if (log_error_) {
      log_error_(std::string("Control protocol send failed: ") + label);
    }
    return false;
  }
  return true;
}

}  // 命名空间 rtc_vehicle
