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
<<<<<<< HEAD
=======
    peer.view_control_channel_open = false;
>>>>>>> 5c8f59e (新加双目和环路切换)
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

<<<<<<< HEAD
=======
void VehicleControlTargetRegistry::SetViewControlChannelOpen(
    uint32_t sessionid,
    bool open) {
  if (sessionid == 0) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  PeerState& peer = peers_[sessionid];
  peer.view_control_channel_open = open;
}

>>>>>>> 5c8f59e (新加双目和环路切换)
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

<<<<<<< HEAD
=======
uint32_t VehicleControlTargetRegistry::view_control_target() const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto selected_it = peers_.find(selected_target_);
  if (selected_target_ != 0 && selected_it != peers_.end() &&
      selected_it->second.connected &&
      selected_it->second.view_control_channel_open) {
    return selected_target_;
  }

  for (const auto& entry : peers_) {
    if (entry.second.connected && entry.second.view_control_channel_open) {
      return entry.first;
    }
  }
  return 0;
}

>>>>>>> 5c8f59e (新加双目和环路切换)
}  // namespace rtc_console
