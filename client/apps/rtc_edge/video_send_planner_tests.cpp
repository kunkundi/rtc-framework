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

void CheckMode(const rtc_edge_app::VideoSendPlan& plan,
               vts_rtc::vision::ViewMode mode) {
  using vts_rtc::vision::ViewMode;
  Check(plan.stereo.continuous == (mode == ViewMode::Binocular), "stereo selection");
  Check(plan.grid.continuous == (mode == ViewMode::Binocular || mode == ViewMode::Surround), "grid selection");
  Check(plan.front.continuous == (mode == ViewMode::SurroundFront), "front selection");
  Check(plan.rear.continuous == (mode == ViewMode::SurroundRear), "rear selection");
  Check(plan.left.continuous == (mode == ViewMode::SurroundLeft), "left selection");
  Check(plan.right.continuous == (mode == ViewMode::SurroundRight), "right selection");
  Check(!plan.stereo.send_once && !plan.grid.send_once && !plan.front.send_once &&
        !plan.rear.send_once && !plan.left.send_once && !plan.right.send_once, "no inactive frames");
}

void TestBinocularViewDoesNotLimitFrameRate() {
  rtc_edge_app::VideoSendPlanner planner(std::chrono::milliseconds(1000));
  const std::chrono::steady_clock::time_point start(
      std::chrono::milliseconds(100));

  CheckMode(planner.Next(vts_rtc::vision::ViewMode::Binocular, start), vts_rtc::vision::ViewMode::Binocular);
}

void TestSurroundViewDoesNotLimitFrameRate() {
  rtc_edge_app::VideoSendPlanner planner(std::chrono::milliseconds(1000));
  const std::chrono::steady_clock::time_point start(
      std::chrono::milliseconds(100));

  CheckMode(planner.Next(vts_rtc::vision::ViewMode::Surround, start), vts_rtc::vision::ViewMode::Surround);
}

void TestViewSwitchStopsInactiveStreams() {
  rtc_edge_app::VideoSendPlanner planner(std::chrono::milliseconds(1000));
  const std::chrono::steady_clock::time_point start(
      std::chrono::milliseconds(100));

  for (uint32_t value = 0; value <= 7; ++value) {
    const auto mode = static_cast<vts_rtc::vision::ViewMode>(value);
    CheckMode(planner.Next(mode, start), mode);
    planner.Reset();
  }
}

}  // 匿名命名空间

int main() {
  TestBinocularViewDoesNotLimitFrameRate();
  TestSurroundViewDoesNotLimitFrameRate();
  TestViewSwitchStopsInactiveStreams();
  std::cout << "rtc_edge_streaming_tests passed" << std::endl;
  return 0;
}
