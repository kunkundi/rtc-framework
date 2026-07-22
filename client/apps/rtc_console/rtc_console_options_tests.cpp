#include "rtc_console_options.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
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

void WriteConfig(const std::string& path, const std::string& content) {
  std::ofstream output(path);
  Check(output.is_open(), "open temporary config");
  output << content;
  Check(static_cast<bool>(output), "write temporary config");
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
  Check(options.room_id_from_command_line,
        "mark command-line room override");
  Check(options.room_id == "test-room", "parse room");

  const rtc_console::AppOptions alias =
      Parse({"rtc_console", "--headless"});
  Check(alias.no_render, "accept headless alias");
}

void TestConfig() {
  const std::string path = "rtc_console_options_test_config.json";

  WriteConfig(path,
              "{\"console\":{\"room\":\"configured-room\","
              "\"auto_open_room\":true}}");
  const rtc_console::AppConfig enabled_config = rtc_console::LoadConfig(path);
  Check(enabled_config.room_id == "configured-room",
        "load configured room");
  Check(enabled_config.auto_open_room,
        "load enabled room auto-open");

  WriteConfig(path,
              "{\"console\":{\"room\":\"other-room\","
              "\"auto_open_room\":false}}");
  const rtc_console::AppConfig disabled_config = rtc_console::LoadConfig(path);
  Check(disabled_config.room_id == "other-room",
        "load second configured room");
  Check(!disabled_config.auto_open_room,
        "load disabled room auto-open");

  WriteConfig(path, "{}");
  const rtc_console::AppConfig default_config = rtc_console::LoadConfig(path);
  Check(default_config.room_id == "zhejianglab", "default configured room");
  Check(!default_config.auto_open_room,
        "default room auto-open to disabled");

  WriteConfig(path, "{\"console\":{\"room\":1}}");
  bool invalid_room_rejected = false;
  try {
    rtc_console::LoadConfig(path);
  } catch (const std::runtime_error& ex) {
    invalid_room_rejected =
        std::string(ex.what()).find("console.room") != std::string::npos;
  }
  Check(invalid_room_rejected, "reject invalid configured room");

  WriteConfig(path, "{\"console\":{\"auto_open_room\":1}}");
  bool invalid_type_rejected = false;
  try {
    rtc_console::LoadConfig(path);
  } catch (const std::runtime_error& ex) {
    invalid_type_rejected =
        std::string(ex.what()).find("console.auto_open_room") !=
        std::string::npos;
  }
  Check(invalid_type_rejected, "reject non-boolean room auto-open");
  std::remove(path.c_str());
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
  TestConfig();
  std::cout << "rtc_console options tests passed" << std::endl;
  return 0;
}
