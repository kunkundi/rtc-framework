#pragma once

#include "rtc_vehicle_protocol/vehicle_control_protocol.h"

#include <cstdint>
#include <map>
#include <mutex>
#include <vector>

namespace rtc_console {

struct VehicleControlInput {
  bool forward = false;
  bool reverse = false;
  bool left = false;
  bool right = false;
  bool emergency_stop = false;
  float throttle = 0.0f;
};

float ClampVehicleThrottle(float throttle);
vts_rtc::vehicle::DriveCommand MakeVehicleDriveCommand(
    const VehicleControlInput& input);

class VehicleControlTargetRegistry {
 public:
  void Clear();
  // 断开的会话为当前选择目标时返回 true。
  bool SetP2PConnected(uint32_t sessionid, bool connected);
  void SetControlChannelOpen(uint32_t sessionid, bool open);
  std::vector<uint32_t> AvailableTargets() const;
  bool SelectTarget(uint32_t sessionid);
  uint32_t selected_target() const;
  bool selected_target_ready() const;

 private:
  struct PeerState {
    bool connected = false;
    bool control_channel_open = false;
  };

  mutable std::mutex mutex_;
  std::map<uint32_t, PeerState> peers_;
  uint32_t selected_target_ = 0;
};

}  // namespace rtc_console
