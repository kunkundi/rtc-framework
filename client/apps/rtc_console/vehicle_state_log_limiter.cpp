#include "vehicle_state_log_limiter.h"

namespace rtc_console {

VehicleStateLogLimiter::VehicleStateLogLimiter(
    std::chrono::milliseconds interval)
    : interval_(interval) {}

bool VehicleStateLogLimiter::ShouldLog(uint64_t remote_session_id,
                                       uint32_t active_gear,
                                       bool watchdog_stopped,
                                       Clock::time_point now) {
  const auto it = session_states_.find(remote_session_id);
  if (it == session_states_.end()) {
    SessionState state;
    state.active_gear = active_gear;
    state.watchdog_stopped = watchdog_stopped;
    state.last_logged_at = now;
    session_states_[remote_session_id] = state;
    return true;
  }

  SessionState& state = it->second;
  const bool safety_state_changed =
      state.active_gear != active_gear ||
      state.watchdog_stopped != watchdog_stopped;
  const bool interval_elapsed = now < state.last_logged_at ||
                                now - state.last_logged_at >= interval_;
  if (!safety_state_changed && !interval_elapsed) {
    return false;
  }

  state.active_gear = active_gear;
  state.watchdog_stopped = watchdog_stopped;
  state.last_logged_at = now;
  return true;
}

}  // 命名空间 rtc_console
