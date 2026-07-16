#pragma once

#include <string>

namespace rtc_console {

struct AppOptions {
  bool no_render = false;
  bool show_help = false;
  std::string room_id = "zhejianglab";
};

AppOptions ParseOptions(int argc, char** argv);
void PrintUsage(const char* program);

}
