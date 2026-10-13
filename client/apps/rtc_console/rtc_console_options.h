#pragma once

#include <array>
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
  std::array<std::string, 4> device_slots{{"car_1", "car_2", "car_3", "car_4"}};
  double steering_wheel_range_deg = 900.0;
  bool steering_servo_calibrated = false;
  double steering_servo_left_deg = 0.0;
  double steering_servo_center_deg = 0.0;
  double steering_servo_right_deg = 0.0;
};

AppOptions ParseOptions(int argc, char** argv);
AppConfig LoadConfig(const std::string& config_path);
void PrintUsage(const char* program);

}
