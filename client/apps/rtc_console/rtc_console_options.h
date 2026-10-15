#pragma once

#include <array>
#include <map>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace rtc_console {

struct AppOptions {
  bool no_render = false;
  bool show_help = false;
  bool room_id_from_command_line = false;
  std::string room_id = "zhejianglab";
};

struct AppConfig {
  nlohmann::json rtc_settings = nlohmann::json::object();
  bool yolo_enabled = true;
  int yolo_max_fps = 15;
  std::string yolo_model_path;
  std::map<std::string, std::vector<std::string>> key_bindings{
      {"toggle_interleave", {"I"}}, {"toggle_view", {"S"}},
      {"select_1", {"1"}}, {"select_2", {"2"}},
      {"select_3", {"3"}}, {"select_4", {"4"}},
      {"takeover", {"C"}}, {"release_takeover", {"Q"}},
      {"brake_takeover", {"B"}}, {"stop_and_release", {"X"}},
      {"gear_forward", {"D"}}, {"gear_neutral", {"N"}},
      {"gear_reverse", {"R"}}, {"throttle_up", {"Equal"}},
      {"throttle_down", {"Minus"}}, {"forward", {"UpArrow"}},
      {"reverse", {"DownArrow"}}, {"left", {"LeftArrow"}},
      {"right", {"RightArrow"}}, {"emergency_stop", {"Space"}},
      {"escape", {"Escape"}}};
  std::string room_id = "zhejianglab";
  bool auto_open_room = false;
  std::array<std::string, 4> device_slots{{"car_1", "car_2", "car_3", "car_4"}};
  double steering_wheel_range_deg = 900.0;
  bool steering_servo_calibrated = false;
  double steering_servo_left_deg = 0.0;
  double steering_servo_center_deg = 0.0;
  double steering_servo_right_deg = 0.0;
};

AppOptions ParseOptions(int argc, char** argv);
AppConfig LoadConfig(const std::string& config_path);
void SaveConfig(const std::string& config_path, const AppConfig& config);
void PrintUsage(const char* program);

}
