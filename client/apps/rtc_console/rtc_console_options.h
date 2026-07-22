#pragma once

#include <string>

namespace rtc_console {

struct AppOptions {
  bool no_render = false;
  bool show_help = false;
  bool room_id_from_command_line = false;
  std::string room_id = "zhejianglab";
};

struct AppConfig {
  std::string room_id = "zhejianglab";
  bool auto_open_room = false;
};

AppOptions ParseOptions(int argc, char** argv);
AppConfig LoadConfig(const std::string& config_path);
void PrintUsage(const char* program);

}
