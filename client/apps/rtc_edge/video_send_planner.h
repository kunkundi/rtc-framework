#pragma once

#include <chrono>
#include "rtc_vision/view_control_protocol.h"

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
  CameraSendPlan grid;
};

class VideoSendPlanner {
 public:
  explicit VideoSendPlanner(std::chrono::milliseconds inactive_interval);

  VideoSendPlan Next(vts_rtc::vision::ViewMode mode,
                     std::chrono::steady_clock::time_point now);
  void Reset();
};

}  // 命名空间 rtc_edge_app
