#include "video_view_state.h"

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

void TestSessionIsolation() {
  rtc_console::VideoViewState state;
  Check(state.Get(101) == vts_rtc::vision::ViewMode::Binocular,
        "default first session to binocular");
  Check(state.Get(202) == vts_rtc::vision::ViewMode::Binocular,
        "default second session to binocular");

  state.Set(101, vts_rtc::vision::ViewMode::Surround);
  Check(state.Get(101) == vts_rtc::vision::ViewMode::Surround,
        "switch selected session to surround");
  Check(state.Get(202) == vts_rtc::vision::ViewMode::Binocular,
        "keep other session binocular");
}

void TestEraseAndClear() {
  rtc_console::VideoViewState state;
  state.Set(101, vts_rtc::vision::ViewMode::Surround);
  state.Set(202, vts_rtc::vision::ViewMode::Surround);

  state.Erase(101);
  Check(state.Get(101) == vts_rtc::vision::ViewMode::Binocular,
        "reset disconnected session");
  Check(state.Get(202) == vts_rtc::vision::ViewMode::Surround,
        "preserve connected session");

  state.Clear();
  Check(state.Get(202) == vts_rtc::vision::ViewMode::Binocular,
        "reset all sessions");
}

void TestInvalidState() {
  rtc_console::VideoViewState state;
  state.Set(0, vts_rtc::vision::ViewMode::Surround);
  state.Set(101, vts_rtc::vision::ViewMode::Unknown);
  Check(state.Get(0) == vts_rtc::vision::ViewMode::Binocular,
        "ignore zero session");
  Check(state.Get(101) == vts_rtc::vision::ViewMode::Binocular,
        "ignore unknown view mode");
}

}  // 匿名命名空间

int main() {
  TestSessionIsolation();
  TestEraseAndClear();
  TestInvalidState();
  std::cout << "rtc_console video view state tests passed" << std::endl;
  return 0;
}
