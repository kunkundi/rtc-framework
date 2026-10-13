#include "video_send_planner.h"

namespace rtc_edge_app {

VideoSendPlanner::VideoSendPlanner(
    std::chrono::milliseconds inactive_interval) {
  (void)inactive_interval;
}

VideoSendPlan VideoSendPlanner::Next(
    vts_rtc::vision::ViewMode mode,
    std::chrono::steady_clock::time_point now) {
  (void)now;
  VideoSendPlan plan;
  using vts_rtc::vision::ViewMode;
  plan.stereo.continuous = mode == ViewMode::Binocular;
  plan.grid.continuous = mode == ViewMode::Binocular || mode == ViewMode::Surround;
  plan.front.continuous = mode == ViewMode::SurroundFront;
  plan.rear.continuous = mode == ViewMode::SurroundRear;
  plan.left.continuous = mode == ViewMode::SurroundLeft;
  plan.right.continuous = mode == ViewMode::SurroundRight;
  return plan;
}

void VideoSendPlanner::Reset() {}

}  // 命名空间 rtc_edge_app
