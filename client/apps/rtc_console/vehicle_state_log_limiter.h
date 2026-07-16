#pragma once

#include <chrono>
#include <cstdint>
#include <map>

namespace rtc_console {

class VehicleStateLogLimiter {
 public:
  using Clock = std::chrono::steady_clock;

  explicit VehicleStateLogLimiter(
      std::chrono::milliseconds interval = std::chrono::milliseconds(1000));

  bool ShouldLog(uint64_t remote_session_id,
                 uint32_t active_gear,
                 bool watchdog_stopped,
                 Clock::time_point now);

 private:
  struct SessionState {
    uint32_t active_gear = 0;
    bool watchdog_stopped = false;
    Clock::time_point last_logged_at{};
  };

  std::chrono::milliseconds interval_;
  std::map<uint64_t, SessionState> session_states_;
};

}  // 命名空间 rtc_console
