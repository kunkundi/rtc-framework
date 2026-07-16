#pragma once

#include <string>

namespace rtc_p2p_imgui {

struct AppOptions {
  bool no_render = false;
  bool show_help = false;
  std::string room_id = "zhejianglab";
};

AppOptions ParseOptions(int argc, char** argv);
void PrintUsage(const char* program);

}
