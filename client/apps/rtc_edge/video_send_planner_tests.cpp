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

void CheckSendOnce(const rtc_edge_app::CameraSendPlan& plan,
                   const std::string& name) {
  Check(!plan.continuous, name + " should be inactive");
  Check(plan.send_once, name + " should queue one low-rate frame");
}

void CheckInactive(const rtc_edge_app::CameraSendPlan& plan,
                   const std::string& name) {
  Check(!plan.continuous, name + " should be inactive");
  Check(!plan.send_once, name + " should not queue another frame yet");
}

void TestBinocularPlanAndInactiveCadence() {
  rtc_edge_app::VideoSendPlanner planner(std::chrono::milliseconds(1000));
  const std::chrono::steady_clock::time_point start(
      std::chrono::milliseconds(100));

  rtc_edge_app::VideoSendPlan plan = planner.Next(true, start);
  CheckContinuous(plan.stereo, "stereo");
  CheckSendOnce(plan.front, "front");
  CheckSendOnce(plan.rear, "rear");
  CheckSendOnce(plan.left, "left");
  CheckSendOnce(plan.right, "right");

  plan = planner.Next(true, start + std::chrono::milliseconds(999));
  CheckContinuous(plan.stereo, "stereo");
  CheckInactive(plan.front, "front");
  CheckInactive(plan.rear, "rear");
  CheckInactive(plan.left, "left");
  CheckInactive(plan.right, "right");

  plan = planner.Next(true, start + std::chrono::milliseconds(1000));
  CheckSendOnce(plan.front, "front");
  CheckSendOnce(plan.rear, "rear");
  CheckSendOnce(plan.left, "left");
  CheckSendOnce(plan.right, "right");
}

void TestViewSwitchKeepsIndependentCadence() {
  rtc_edge_app::VideoSendPlanner planner(std::chrono::milliseconds(1000));
  const std::chrono::steady_clock::time_point start(
      std::chrono::milliseconds(100));

  planner.Next(true, start);
  rtc_edge_app::VideoSendPlan plan =
      planner.Next(false, start + std::chrono::milliseconds(100));
  CheckSendOnce(plan.stereo, "stereo");
  CheckContinuous(plan.front, "front");
  CheckContinuous(plan.rear, "rear");
  CheckContinuous(plan.left, "left");
  CheckContinuous(plan.right, "right");

  plan = planner.Next(false, start + std::chrono::milliseconds(1099));
  CheckInactive(plan.stereo, "stereo");
  plan = planner.Next(false, start + std::chrono::milliseconds(1100));
  CheckSendOnce(plan.stereo, "stereo");

  plan = planner.Next(true, start + std::chrono::milliseconds(1200));
  CheckContinuous(plan.stereo, "stereo");
  CheckSendOnce(plan.front, "front");
  CheckSendOnce(plan.rear, "rear");
  CheckSendOnce(plan.left, "left");
  CheckSendOnce(plan.right, "right");
}

void TestResetRestartsInactiveSchedule() {
  rtc_edge_app::VideoSendPlanner planner(std::chrono::milliseconds(1000));
  const std::chrono::steady_clock::time_point start(
      std::chrono::milliseconds(100));

  planner.Next(true, start);
  rtc_edge_app::VideoSendPlan plan =
      planner.Next(true, start + std::chrono::milliseconds(100));
  CheckInactive(plan.front, "front before reset");

  planner.Reset();
  plan = planner.Next(true, start + std::chrono::milliseconds(100));
  CheckSendOnce(plan.front, "front after reset");
}

}  // 匿名命名空间

int main() {
  TestBinocularPlanAndInactiveCadence();
  TestViewSwitchKeepsIndependentCadence();
  TestResetRestartsInactiveSchedule();
  std::cout << "rtc_edge_streaming_tests passed" << std::endl;
  return 0;
}
