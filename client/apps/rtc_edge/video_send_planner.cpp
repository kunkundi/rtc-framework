#include "video_send_planner.h"

namespace rtc_edge_app {

VideoSendPlanner::VideoSendPlanner(
    std::chrono::milliseconds inactive_interval)
    : inactive_interval_(inactive_interval) {}

VideoSendPlan VideoSendPlanner::Next(
    bool binocular_enabled,
    std::chrono::steady_clock::time_point now) {
  VideoSendPlan plan;
  plan.stereo.continuous = binocular_enabled;
  plan.front.continuous = !binocular_enabled;
  plan.rear.continuous = !binocular_enabled;
  plan.left.continuous = !binocular_enabled;
  plan.right.continuous = !binocular_enabled;

  if (binocular_enabled) {
    plan.front.send_once = ShouldSendInactive(now, &front_inactive_);
    plan.rear.send_once = ShouldSendInactive(now, &rear_inactive_);
    plan.left.send_once = ShouldSendInactive(now, &left_inactive_);
    plan.right.send_once = ShouldSendInactive(now, &right_inactive_);
  } else {
    plan.stereo.send_once = ShouldSendInactive(now, &stereo_inactive_);
  }
  return plan;
}

void VideoSendPlanner::Reset() {
  stereo_inactive_ = InactiveSendState();
  front_inactive_ = InactiveSendState();
  rear_inactive_ = InactiveSendState();
  left_inactive_ = InactiveSendState();
  right_inactive_ = InactiveSendState();
}

bool VideoSendPlanner::ShouldSendInactive(
    std::chrono::steady_clock::time_point now,
    InactiveSendState* state) {
  if (state == nullptr) {
    return false;
  }
  if (!state->initialized ||
      now - state->last_send >= inactive_interval_) {
    state->initialized = true;
    state->last_send = now;
    return true;
  }
  return false;
}

}  // 命名空间 rtc_edge_app
