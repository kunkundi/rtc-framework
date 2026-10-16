#include "video_fullscreen_state.h"

#include <cstdlib>
#include <iostream>
#include <initializer_list>
#include <limits>

namespace {

void Check(bool condition, const char* message) {
  if (!condition) {
    std::cerr << message << std::endl;
    std::exit(1);
  }
}

void TestReturnToOriginalView() {
  rtc_console::VideoFullscreenState state;
  const rtc_console::FullscreenVideoCrop full{};
  state.OpenStream("101:stereo", full, "双目");
  state.Back();
  Check(!state.active(), "单路全屏退出后应恢复窗口模式");

  state.OpenMonitor();
  state.OpenStream("101:stereo", full, "双目");
  state.OpenStream("202:stereo", full, "另一设备");
  state.Back();
  Check(state.mode() == rtc_console::VideoFullscreenState::Mode::Monitor,
        "全屏监看中切换单路后应返回全屏监看");
  Check(state.stream_key().empty(), "返回监看后不能保留旧媒体流");
  state.Back();
  state.Back();
  Check(!state.active(), "退出监看和重复退出应保持窗口模式");
}

void TestSurroundCropAndDisconnectReset() {
  rtc_console::VideoFullscreenState state;
  rtc_console::FullscreenVideoCrop rear;
  rear.left = 0.5f;
  rear.bottom = 0.5f;
  state.OpenMonitor();
  state.OpenStream("101:surround_grid", rear, "后视画面");
  rear.left = 0;
  Check(state.crop().left == 0.5f && state.crop().bottom == 0.5f,
        "环视单路全屏必须保留原有裁剪区域且不依赖调用方对象");
  Check(state.label() == "后视画面", "单路全屏应保留摄像头名称");
  state.Reset();
  Check(!state.active() && state.stream_key().empty() && state.label().empty(),
        "会话重置后不能继续显示旧设备画面");
  Check(state.crop().left == 0 && state.crop().bottom == 1,
        "重置后不能将环视裁剪应用到下一个双目画面");
  state.OpenStream("202:stereo", {}, "双目");
  state.Back();
  Check(!state.active(), "会话重置后不能返回旧的全屏监看状态");
}

void TestInvalidOpenPreservesView() {
  rtc_console::VideoFullscreenState state;
  state.OpenMonitor();
  Check(!state.OpenStream("", {}, "无效画面"), "空媒体流不能进入全屏");
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float inf = std::numeric_limits<float>::infinity();
  for (float value : {nan, inf, -0.1f, 1.0f}) {
    rtc_console::FullscreenVideoCrop invalid;
    invalid.left = value;
    Check(!state.OpenStream("101:stereo", invalid, "无效裁剪"),
          "非法或退化的裁剪区域不能进入全屏");
  }
  Check(state.mode() == rtc_console::VideoFullscreenState::Mode::Monitor,
        "无效全屏请求不能破坏当前监看状态");
}

}

int main() {
  TestReturnToOriginalView();
  TestSurroundCropAndDisconnectReset();
  TestInvalidOpenPreservesView();
  std::cout << "rtc_console video fullscreen state tests passed" << std::endl;
  return 0;
}
