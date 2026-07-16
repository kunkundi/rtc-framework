#include "rtc_console_options.h"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void Check(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << std::endl;
    std::exit(1);
  }
}

rtc_console::AppOptions Parse(const std::vector<std::string>& arguments) {
  std::vector<char*> argv;
  for (const std::string& argument : arguments) {
    argv.push_back(const_cast<char*>(argument.c_str()));
  }
  return rtc_console::ParseOptions(static_cast<int>(argv.size()), argv.data());
}

void TestDefaults() {
  const rtc_console::AppOptions options = Parse({"rtc_console"});
  Check(!options.no_render, "render by default");
  Check(options.room_id == "zhejianglab", "default room");
}

void TestNoRenderAndRoom() {
  const rtc_console::AppOptions options =
      Parse({"rtc_console", "--no-render", "--room", "test-room"});
  Check(options.no_render, "enable no-render mode");
  Check(options.room_id == "test-room", "parse room");

  const rtc_console::AppOptions alias =
      Parse({"rtc_console", "--headless"});
  Check(alias.no_render, "accept headless alias");
}

void TestHelp() {
  const rtc_console::AppOptions options = Parse({"rtc_console", "--help"});
  Check(options.show_help, "parse help");
}

void TestInvalidArguments() {
  bool missing_room_rejected = false;
  try {
    Parse({"rtc_console", "--room"});
  } catch (const std::runtime_error&) {
    missing_room_rejected = true;
  }
  Check(missing_room_rejected, "reject missing room");

  bool unknown_rejected = false;
  try {
    Parse({"rtc_console", "--unknown"});
  } catch (const std::runtime_error&) {
    unknown_rejected = true;
  }
  Check(unknown_rejected, "reject unknown argument");
}

}

int main() {
  TestDefaults();
  TestNoRenderAndRoom();
  TestHelp();
  TestInvalidArguments();
  std::cout << "rtc_console options tests passed" << std::endl;
  return 0;
}
