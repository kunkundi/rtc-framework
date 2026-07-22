#include "rtc_console_options.h"

#include <fstream>
#include <iostream>
#include <stdexcept>

#include <nlohmann/json.hpp>

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
      options.room_id_from_command_line = true;
    } else {
      throw std::runtime_error("unknown argument: " + argument);
    }
  }

  return options;
}

AppConfig LoadConfig(const std::string& config_path) {
  std::ifstream input(config_path);
  if (!input.is_open()) {
    throw std::runtime_error("failed to open config file: " + config_path);
  }

  nlohmann::json root;
  try {
    input >> root;
  } catch (const nlohmann::json::exception& ex) {
    throw std::runtime_error(std::string("failed to parse config JSON: ") +
                             ex.what());
  }
  if (!root.is_object()) {
    throw std::runtime_error("config root must be an object");
  }

  AppConfig config;
  if (!root.contains("console")) {
    return config;
  }

  const nlohmann::json& console = root.at("console");
  if (!console.is_object()) {
    throw std::runtime_error("config field must be an object: console");
  }
  if (console.contains("room")) {
    const nlohmann::json& room = console.at("room");
    if (!room.is_string() || room.get<std::string>().empty()) {
      throw std::runtime_error(
          "config field must be a non-empty string: console.room");
    }
    config.room_id = room.get<std::string>();
  }

  if (console.contains("auto_open_room")) {
    const nlohmann::json& auto_open_room = console.at("auto_open_room");
    if (!auto_open_room.is_boolean()) {
      throw std::runtime_error(
          "config field must be a boolean: console.auto_open_room");
    }
    config.auto_open_room = auto_open_room.get<bool>();
  }
  return config;
}

void PrintUsage(const char* program) {
  std::cout << "Usage: " << program << " [options]\n"
            << "\n"
            << "Options:\n"
            << "  --room <room-id>  Override the configured room ID\n"
            << "  --no-render       Run without creating a window or OpenGL context\n"
            << "  --headless        Alias for --no-render\n"
            << "  --help, -h        Show this message\n";
}

}
