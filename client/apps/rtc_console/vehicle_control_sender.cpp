#include "vehicle_control_sender.h"

#include <algorithm>
#include <cmath>

namespace rtc_console {

float ClampVehicleThrottle(float throttle) {
  if (!std::isfinite(throttle)) {
    return 0.0f;
  }
  return std::max(0.0f, std::min(1.0f, throttle));
}

vts_rtc::vehicle::DriveCommand MakeVehicleDriveCommand(
    const VehicleControlInput& input) {
  vts_rtc::vehicle::DriveCommand command;
  if (input.emergency_stop) {
    command.drive_direction = vts_rtc::vehicle::DriveDirection::Stop;
    command.steering_direction = vts_rtc::vehicle::SteeringDirection::Center;
    command.throttle = 0.0f;
    command.brake = 1.0f;
    return command;
  }

  if (input.forward && !input.reverse) {
    command.drive_direction = vts_rtc::vehicle::DriveDirection::Forward;
  } else if (input.reverse && !input.forward) {
    command.drive_direction = vts_rtc::vehicle::DriveDirection::Reverse;
  } else {
    command.drive_direction = vts_rtc::vehicle::DriveDirection::Stop;
  }

  if (input.left && !input.right) {
    command.steering_direction = vts_rtc::vehicle::SteeringDirection::Left;
  } else if (input.right && !input.left) {
    command.steering_direction = vts_rtc::vehicle::SteeringDirection::Right;
  } else {
    command.steering_direction = vts_rtc::vehicle::SteeringDirection::Center;
  }

  command.throttle = ClampVehicleThrottle(input.throttle);
  command.brake = 0.0f;
  return command;
}

GamepadDogActionState::GamepadDogActionState(
    uint64_t lateral_heartbeat_ms)
    : lateral_heartbeat_ms_(lateral_heartbeat_ms == 0
                                ? 100
                                : lateral_heartbeat_ms) {}

void GamepadDogActionState::Update(bool dpad_left,
                                   bool dpad_right,
                                   bool stand,
                                   bool lie_down,
                                   float speed,
                                   uint64_t now_ms,
                                   const SendCallback& send) {
  if (!send) {
    return;
  }

  const int desired_lateral =
      dpad_left == dpad_right ? 0 : (dpad_left ? -1 : 1);
  const bool lateral_changed = desired_lateral != lateral_;
  const bool heartbeat_due =
      desired_lateral != 0 && now_ms >= last_lateral_sent_ms_ &&
      now_ms - last_lateral_sent_ms_ >= lateral_heartbeat_ms_;
  if (lateral_changed || heartbeat_due) {
    vts_rtc::vehicle::DogActionType action =
        vts_rtc::vehicle::DogActionType::LateralStop;
    if (desired_lateral < 0) {
      action = vts_rtc::vehicle::DogActionType::LateralLeft;
    } else if (desired_lateral > 0) {
      action = vts_rtc::vehicle::DogActionType::LateralRight;
    }
    if (send(action, speed, lateral_changed)) {
      lateral_ = desired_lateral;
      last_lateral_sent_ms_ = now_ms;
    }
  }

  if (!stand) {
    stand_ = false;
  } else if (!stand_ &&
             send(vts_rtc::vehicle::DogActionType::Stand, 0.0f, true)) {
    stand_ = true;
  }
  if (!lie_down) {
    lie_down_ = false;
  } else if (!lie_down_ &&
             send(vts_rtc::vehicle::DogActionType::LieDown, 0.0f, true)) {
    lie_down_ = true;
  }
}

bool GamepadDogActionState::StopForDisconnect(const SendCallback& send) {
  stand_ = false;
  lie_down_ = false;
  if (lateral_ == 0) {
    return true;
  }
  if (!send ||
      !send(vts_rtc::vehicle::DogActionType::LateralStop, 0.0f, true)) {
    return false;
  }
  lateral_ = 0;
  last_lateral_sent_ms_ = 0;
  return true;
}

void GamepadDogActionState::Reset() {
  lateral_ = 0;
  last_lateral_sent_ms_ = 0;
  stand_ = false;
  lie_down_ = false;
}

void VehicleEventAckTracker::TrackDogAction(
    uint64_t request_id,
    vts_rtc::vehicle::DogActionType action,
    bool log_result) {
  if (request_id == 0) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  dog_actions_[request_id] = {action, log_result};
}

bool VehicleEventAckTracker::ResolveDogAction(
    uint64_t request_id,
    DogActionAckContext* context) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto pending = dog_actions_.find(request_id);
  if (pending == dog_actions_.end()) {
    return false;
  }
  if (context) {
    *context = pending->second;
  }
  dog_actions_.erase(pending);
  return true;
}

void VehicleEventAckTracker::Clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  dog_actions_.clear();
}

void VehicleControlTargetRegistry::Clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  peers_.clear();
  selected_target_ = 0;
}

bool VehicleControlTargetRegistry::SetP2PConnected(
    uint32_t sessionid,
    bool connected) {
  if (sessionid == 0) {
    return false;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  PeerState& peer = peers_[sessionid];
  peer.connected = connected;
  if (!connected) {
    peer.control_channel_open = false;
    const bool selected_disconnected = selected_target_ == sessionid;
    if (selected_target_ == sessionid) {
      selected_target_ = 0;
    }
    peers_.erase(sessionid);
    return selected_disconnected;
  }
  return false;
}

void VehicleControlTargetRegistry::SetControlChannelOpen(
    uint32_t sessionid,
    bool open) {
  if (sessionid == 0) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  PeerState& peer = peers_[sessionid];
  peer.control_channel_open = open;
}

std::vector<uint32_t>
VehicleControlTargetRegistry::AvailableTargets() const {
  std::vector<uint32_t> targets;
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto& entry : peers_) {
    if (entry.second.connected && entry.second.control_channel_open) {
      targets.push_back(entry.first);
    }
  }
  return targets;
}

bool VehicleControlTargetRegistry::SelectTarget(uint32_t sessionid) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (sessionid == 0) {
    selected_target_ = 0;
    return true;
  }
  const auto it = peers_.find(sessionid);
  if (it == peers_.end() || !it->second.connected ||
      !it->second.control_channel_open) {
    return false;
  }
  selected_target_ = sessionid;
  return true;
}

uint32_t VehicleControlTargetRegistry::selected_target() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return selected_target_;
}

bool VehicleControlTargetRegistry::selected_target_ready() const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = peers_.find(selected_target_);
  return selected_target_ != 0 && it != peers_.end() &&
         it->second.connected && it->second.control_channel_open;
}

}  // namespace rtc_console
