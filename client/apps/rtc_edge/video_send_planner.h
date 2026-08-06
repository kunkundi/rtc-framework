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

 private:
  struct InactiveSendState {
    bool initialized = false;
    std::chrono::steady_clock::time_point last_send{};
  };

  bool ShouldSendInactive(std::chrono::steady_clock::time_point now,
                          InactiveSendState* state);

  const std::chrono::milliseconds inactive_interval_;
  InactiveSendState stereo_inactive_;
  InactiveSendState front_inactive_;
  InactiveSendState rear_inactive_;
  InactiveSendState left_inactive_;
  InactiveSendState right_inactive_;
};

}  // 命名空间 rtc_edge_app
