#include "video_send_planner.h"

namespace rtc_edge_app {

VideoSendPlanner::VideoSendPlanner(
    std::chrono::milliseconds inactive_interval) {
  (void)inactive_interval;
}

VideoSendPlan VideoSendPlanner::Next(
    bool binocular_enabled,
    std::chrono::steady_clock::time_point now) {
  (void)binocular_enabled;
  (void)now;
  VideoSendPlan plan;
  plan.stereo.continuous = true;
  plan.front.continuous = true;
  plan.rear.continuous = true;
  plan.left.continuous = true;
  plan.right.continuous = true;
  return plan;
}

void VideoSendPlanner::Reset() {}

}  // 命名空间 rtc_edge_app
