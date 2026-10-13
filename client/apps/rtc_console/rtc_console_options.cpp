#include "rtc_console_options.h"
#include "rtc_runtime/device_identity.h"

#include <fstream>
#include <cmath>
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
  if (console.contains("device_slots")) {
    const auto& slots = console.at("device_slots");
    if (!slots.is_array() || slots.size() != 4) {
      throw std::runtime_error("console.device_slots must contain four IDs");
    }
    for (size_t index = 0; index < config.device_slots.size(); ++index) {
      if (!slots[index].is_string()) {
        throw std::runtime_error("console.device_slots must contain strings");
      }
      const auto id = slots[index].get<std::string>();
      if (!id.empty() && !rtc_runtime::IsValidDeviceId(id)) {
        throw std::runtime_error("invalid ID in console.device_slots");
      }
      for (size_t previous = 0; previous < index; ++previous) {
        if (!id.empty() && config.device_slots[previous] == id) {
          throw std::runtime_error("duplicate ID in console.device_slots");
        }
      }
      config.device_slots[index] = id;
    }
  }
  if (console.contains("steering")) {
    const auto& steering = console.at("steering");
    if (!steering.is_object()) {
      throw std::runtime_error("console.steering must be an object");
    }
    const auto read_angle = [](const nlohmann::json& object,
                               const char* key) {
      if (!object.contains(key) || !object.at(key).is_number()) {
        throw std::runtime_error(std::string("invalid console.steering angle: ") + key);
      }
      const double angle = object.at(key).get<double>();
      if (!std::isfinite(angle)) {
        throw std::runtime_error(std::string("non-finite console.steering angle: ") + key);
      }
      return angle;
    };
    if (steering.contains("wheel_range_deg")) {
      config.steering_wheel_range_deg = read_angle(steering, "wheel_range_deg");
      if (config.steering_wheel_range_deg <= 0.0) {
        throw std::runtime_error("console.steering.wheel_range_deg must be positive");
      }
    }
    if (steering.contains("servo")) {
      const auto& servo = steering.at("servo");
      if (!servo.is_object()) {
        throw std::runtime_error("console.steering.servo must be an object");
      }
      config.steering_servo_left_deg = read_angle(servo, "left_deg");
      config.steering_servo_center_deg = read_angle(servo, "center_deg");
      config.steering_servo_right_deg = read_angle(servo, "right_deg");
      const double left = config.steering_servo_left_deg;
      const double center = config.steering_servo_center_deg;
      const double right = config.steering_servo_right_deg;
      if (!((left < center && center < right) ||
            (right < center && center < left))) {
        throw std::runtime_error("console.steering.servo center must lie strictly between limits");
      }
      config.steering_servo_calibrated = true;
    }
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
