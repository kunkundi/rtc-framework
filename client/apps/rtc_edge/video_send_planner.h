#pragma once

#include <chrono>

namespace rtc_edge_app {

struct CameraSendPlan {
  bool continuous = false;
  bool send_once = false;
};

struct VideoSendPlan {
  CameraSendPlan stereo;
  CameraSendPlan front;
  CameraSendPlan rear;
  CameraSendPlan left;
  CameraSendPlan right;
};

class VideoSendPlanner {
 public:
  explicit VideoSendPlanner(std::chrono::milliseconds inactive_interval);

  VideoSendPlan Next(bool binocular_enabled,
                     std::chrono::steady_clock::time_point now);
  void Reset();
};

}  // 命名空间 rtc_edge_app
