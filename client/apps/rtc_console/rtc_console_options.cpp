#include "rtc_console_options.h"
#include "rtc_runtime/device_identity.h"
#include "rtc_runtime/config_section.h"

#include <fstream>
#include <cmath>
#include <iostream>
#include <set>
#include <stdexcept>
#include <cstdio>

#if defined(_WIN32)
#include <windows.h>
#endif

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

static AppConfig ParseConfig(const nlohmann::json& root) {
  AppConfig config;
  config.rtc_settings = root;
  config.rtc_settings.erase("console");
  config.rtc_settings.erase("edge");
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

AppConfig LoadConfig(const std::string& config_path) {
  std::ifstream input(config_path);
  if (!input.is_open()) {
    throw std::runtime_error("failed to open config file: " + config_path);
  }
  nlohmann::json root;
  input >> root;
  if (!root.is_object()) {
    throw std::runtime_error("config root must be an object");
  }
  rtc_runtime::LoadConfigSection(root, "console", config_path);
  return ParseConfig(root);
}

void SaveConfig(const std::string& config_path, const AppConfig& config) {
  if (!config.rtc_settings.is_object() ||
      config.rtc_settings.contains("console") || config.rtc_settings.contains("edge")) {
    throw std::runtime_error("RTC settings must be an object without application sections");
  }
  nlohmann::json section = {
      {"room", config.room_id}, {"auto_open_room", config.auto_open_room},
      {"device_slots", config.device_slots},
      {"key_bindings", config.key_bindings},
      {"yolo", {{"enabled", config.yolo_enabled},
                {"max_fps", config.yolo_max_fps},
                {"model", config.yolo_model_path}}},
      {"steering", {{"wheel_range_deg", config.steering_wheel_range_deg}}}};
  if (config.steering_servo_calibrated) {
    section["steering"]["servo"] = {
        {"left_deg", config.steering_servo_left_deg},
        {"center_deg", config.steering_servo_center_deg},
        {"right_deg", config.steering_servo_right_deg}};
  }
  ParseConfig(nlohmann::json{{"console", section}});
  std::ifstream input(config_path);
  if (!input.is_open()) {
    throw std::runtime_error("failed to open config file: " + config_path);
  }
  nlohmann::json root;
  input >> root;
  if (!root.is_object()) {
    throw std::runtime_error("config root must be an object");
  }
  for (auto entry = config.rtc_settings.begin(); entry != config.rtc_settings.end(); ++entry) {
    if (root.contains(entry.key()) && root.at(entry.key()).type() != entry.value().type() &&
        !(root.at(entry.key()).is_number() && entry.value().is_number())) {
      throw std::runtime_error("invalid RTC setting type: " + entry.key());
    }
  }
  const auto validate_values = [](const nlohmann::json& value,
                                  const std::string& name,
                                  const auto& recurse) -> void {
    if (value.is_number() && !std::isfinite(value.get<double>())) {
      throw std::runtime_error("non-finite RTC setting: " + name);
    }
    if (value.is_number() && value.get<double>() < 0.0) {
      throw std::runtime_error("negative RTC setting: " + name);
    }
    if (value.is_structured()) {
      for (auto child = value.begin(); child != value.end(); ++child) {
        recurse(child.value(), name + "." + (value.is_object() ? child.key() : "item"), recurse);
      }
    }
  };
  validate_values(config.rtc_settings, "rtc", validate_values);
  for (const auto& field : {"api_server", "signaling_server"}) {
    if (config.rtc_settings.contains(field) &&
        (!config.rtc_settings.at(field).is_string() ||
         config.rtc_settings.at(field).get<std::string>().empty())) {
      throw std::runtime_error(std::string("empty or invalid RTC setting: ") + field);
    }
  }
  root.update(config.rtc_settings);
  nlohmann::json resolved = root;
  rtc_runtime::LoadConfigSection(resolved, "console", config_path);
  std::string destination = config_path;
  const bool external = root.contains("console") &&
                        root.at("console").contains("config");
  if (external) {
    const std::string reference = root.at("console").at("config").get<std::string>();
    const bool absolute = !reference.empty() &&
        (reference.front() == '/' || reference.front() == '\\' ||
         (reference.size() > 1 && reference[1] == ':'));
    const auto separator = config_path.find_last_of("/\\");
    destination = absolute || separator == std::string::npos
        ? reference : config_path.substr(0, separator + 1) + reference;
  }
  nlohmann::json merged = resolved.value("console", nlohmann::json::object());
  merged.merge_patch(section);
  if (!config.steering_servo_calibrated) {
    merged["steering"].erase("servo");
  }
  nlohmann::json main_root = root;
  if (external) {
    root = std::move(merged);
  } else {
    root["console"] = std::move(merged);
  }
  ParseConfig(nlohmann::json{{"console", external ? root : root.at("console")}});
  input.close();
  const std::string temporary = destination + ".tmp";
  {
    std::ofstream output(temporary, std::ios::out | std::ios::trunc);
    if (!output.is_open()) {
      throw std::runtime_error("failed to create config file: " + temporary);
    }
    output << root.dump(4) << '\n';
    output.close();
    if (!output) {
      std::remove(temporary.c_str());
      throw std::runtime_error("failed to write config file: " + temporary);
    }
  }
#if defined(_WIN32)
  const bool replaced = MoveFileExA(temporary.c_str(), destination.c_str(),
      MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
  const bool replaced = std::rename(temporary.c_str(), destination.c_str()) == 0;
#endif
  if (!replaced) {
    std::remove(temporary.c_str());
    throw std::runtime_error("failed to replace config file: " + destination);
  }
  if (external) {
    std::ifstream previous(config_path);
    nlohmann::json original;
    previous >> original;
    previous.close();
    if (original != main_root) {
      const std::string main_temporary = config_path + ".tmp";
      std::ofstream output(main_temporary, std::ios::out | std::ios::trunc);
      output << main_root.dump(4) << '\n';
      output.close();
#if defined(_WIN32)
      const bool main_replaced = output && MoveFileExA(main_temporary.c_str(),
          config_path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
#else
      const bool main_replaced = output &&
          std::rename(main_temporary.c_str(), config_path.c_str()) == 0;
#endif
      if (!main_replaced) {
        std::remove(main_temporary.c_str());
        throw std::runtime_error("console saved, but failed to save RTC settings: " + config_path);
      }
    }
  }
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
