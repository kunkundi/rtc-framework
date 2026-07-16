#include "rtc_console_options.h"

#include <iostream>
#include <stdexcept>

namespace rtc_console {

AppOptions ParseOptions(int argc, char** argv) {
  AppOptions options;

  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    if (argument == "--help" || argument == "-h") {
      options.show_help = true;
    } else if (argument == "--no-render" || argument == "--headless") {
      options.no_render = true;
    } else if (argument == "--room") {
      if (i + 1 >= argc) {
        throw std::runtime_error("missing value for --room");
      }
      options.room_id = argv[++i];
      if (options.room_id.empty()) {
        throw std::runtime_error("--room must not be empty");
      }
    } else {
      throw std::runtime_error("unknown argument: " + argument);
    }
  }

  return options;
}

void PrintUsage(const char* program) {
  std::cout << "Usage: " << program << " [options]\n"
            << "\n"
            << "Options:\n"
            << "  --room <room-id>  Room to open after RTC login "
               "(default: zhejianglab)\n"
            << "  --no-render       Run without creating a window or OpenGL context\n"
            << "  --headless        Alias for --no-render\n"
            << "  --help, -h        Show this message\n";
}

}
