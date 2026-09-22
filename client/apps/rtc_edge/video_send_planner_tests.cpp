#include "video_send_planner.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void Check(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << std::endl;
    std::exit(1);
  }
}

void CheckContinuous(const rtc_edge_app::CameraSendPlan& plan,
                     const std::string& name) {
  Check(plan.continuous, name + " should continuously send");
  Check(!plan.send_once, name + " should not queue a low-rate frame");
}

void CheckAllContinuous(const rtc_edge_app::VideoSendPlan& plan) {
  CheckContinuous(plan.stereo, "stereo");
  CheckContinuous(plan.front, "front");
  CheckContinuous(plan.rear, "rear");
  CheckContinuous(plan.left, "left");
  CheckContinuous(plan.right, "right");
}

void TestBinocularViewDoesNotLimitFrameRate() {
  rtc_edge_app::VideoSendPlanner planner(std::chrono::milliseconds(1000));
  const std::chrono::steady_clock::time_point start(
      std::chrono::milliseconds(100));

  CheckAllContinuous(planner.Next(true, start));
  CheckAllContinuous(
      planner.Next(true, start + std::chrono::milliseconds(1000)));
}

void TestSurroundViewDoesNotLimitFrameRate() {
  rtc_edge_app::VideoSendPlanner planner(std::chrono::milliseconds(1000));
  const std::chrono::steady_clock::time_point start(
      std::chrono::milliseconds(100));

  CheckAllContinuous(planner.Next(false, start));
  CheckAllContinuous(
      planner.Next(false, start + std::chrono::milliseconds(1000)));
}

void TestViewSwitchKeepsAllStreamsContinuous() {
  rtc_edge_app::VideoSendPlanner planner(std::chrono::milliseconds(1000));
  const std::chrono::steady_clock::time_point start(
      std::chrono::milliseconds(100));

  CheckAllContinuous(planner.Next(true, start));
  planner.Reset();
  CheckAllContinuous(
      planner.Next(false, start + std::chrono::milliseconds(100)));
}

}  // 匿名命名空间

int main() {
  TestBinocularViewDoesNotLimitFrameRate();
  TestSurroundViewDoesNotLimitFrameRate();
  TestViewSwitchKeepsAllStreamsContinuous();
  std::cout << "rtc_edge_streaming_tests passed" << std::endl;
  return 0;
}
