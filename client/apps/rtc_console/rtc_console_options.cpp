#include "rtc_console_options.h"
#include "rtc_runtime/device_identity.h"
#include "rtc_runtime/config_section.h"

#include <fstream>
#include <cmath>
#include <iostream>
#include <set>
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
  rtc_runtime::LoadConfigSection(root, "console", config_path);

  AppConfig config;
  if (!root.contains("console")) {
    return config;
  }

  const nlohmann::json& console = root.at("console");
  if (!console.is_object()) {
    throw std::runtime_error("config field must be an object: console");
  }
  if (console.contains("yolo")) {
    const auto& yolo = console.at("yolo");
    if (!yolo.is_object()) {
      throw std::runtime_error("console.yolo must be an object");
    }
    if (yolo.contains("enabled")) {
      if (!yolo.at("enabled").is_boolean()) {
        throw std::runtime_error("console.yolo.enabled must be a boolean");
      }
      config.yolo_enabled = yolo.at("enabled").get<bool>();
    }
    if (yolo.contains("max_fps")) {
      if (!yolo.at("max_fps").is_number_integer() ||
          yolo.at("max_fps") < 1 || yolo.at("max_fps") > 240) {
        throw std::runtime_error("console.yolo.max_fps must be in [1, 240]");
      }
      config.yolo_max_fps = yolo.at("max_fps").get<int>();
    }
    if (yolo.contains("model")) {
      if (!yolo.at("model").is_string()) {
        throw std::runtime_error("console.yolo.model must be a string");
      }
      config.yolo_model_path = yolo.at("model").get<std::string>();
    }
  }
  if (console.contains("key_bindings")) {
    const auto& bindings = console.at("key_bindings");
    if (!bindings.is_object()) {
      throw std::runtime_error("console.key_bindings must be an object");
    }
    const std::set<std::string> named_keys{
        "UpArrow", "DownArrow", "LeftArrow", "RightArrow", "Space",
        "Escape", "Enter", "Tab", "Backspace", "Insert", "Delete",
        "Home", "End", "PageUp", "PageDown", "Equal", "Minus",
        "Comma", "Period", "Slash", "Semicolon", "Apostrophe",
        "LeftBracket", "RightBracket", "Backslash", "GraveAccent"};
    for (auto entry = bindings.begin(); entry != bindings.end(); ++entry) {
      auto action = config.key_bindings.find(entry.key());
      if (action == config.key_bindings.end()) {
        throw std::runtime_error("unknown console.key_bindings action: " + entry.key());
      }
      std::vector<std::string> keys;
      if (entry.value().is_string()) {
        keys.push_back(entry.value().get<std::string>());
      } else if (entry.value().is_array() && !entry.value().empty()) {
        for (const auto& key : entry.value()) {
          if (!key.is_string()) {
            throw std::runtime_error("console.key_bindings keys must be strings: " + entry.key());
          }
          keys.push_back(key.get<std::string>());
        }
      } else {
        throw std::runtime_error("console.key_bindings requires a key or non-empty key array: " + entry.key());
      }
      for (const auto& key : keys) {
        const bool character = key.size() == 1 &&
            ((key[0] >= 'A' && key[0] <= 'Z') || (key[0] >= '0' && key[0] <= '9'));
        const bool function = key.size() >= 2 && key[0] == 'F' &&
            (key == "F1" || key == "F2" || key == "F3" || key == "F4" ||
             key == "F5" || key == "F6" || key == "F7" || key == "F8" ||
             key == "F9" || key == "F10" || key == "F11" || key == "F12");
        if (!character && !function && named_keys.count(key) == 0) {
          throw std::runtime_error("invalid console.key_bindings key: " + key);
        }
      }
      action->second = std::move(keys);
    }
    std::set<std::string> assigned;
    for (const auto& action : config.key_bindings) {
      for (const auto& key : action.second) {
        if (!assigned.insert(key).second) {
          throw std::runtime_error("duplicate console.key_bindings key: " + key);
        }
      }
    }
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
