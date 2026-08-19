#pragma once

#include "rtc_vehicle_protocol/vehicle_control_protocol.h"

#include <cstdint>
#include <functional>
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

// 跟踪手柄机器狗动作，仅在发送成功后推进边沿状态。
class GamepadDogActionState {
 public:
  using SendCallback = std::function<bool(
      vts_rtc::vehicle::DogActionType, float, bool)>;

  explicit GamepadDogActionState(uint64_t lateral_heartbeat_ms = 100);

  void Update(bool dpad_left,
              bool dpad_right,
              bool stand,
              bool lie_down,
              float speed,
              uint64_t now_ms,
              const SendCallback& send);
  // 手柄断开时重复调用，直到横移停止成功发送。
  bool StopForDisconnect(const SendCallback& send);
  void Reset();

  bool lateral_active() const { return lateral_ != 0; }

 private:
  uint64_t lateral_heartbeat_ms_ = 100;
  int lateral_ = 0;
  uint64_t last_lateral_sent_ms_ = 0;
  bool stand_ = false;
  bool lie_down_ = false;
};

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
