#include "edge_options.h"

#include "rtc_vehicle_protocol/vehicle_control_protocol.h"

#include <stdint.h>

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

namespace rtc_edge_headless {
namespace {

std::string MakeConfigPath(const std::string& parent, const char* key) {
  if (parent.empty()) {
    return key;
  }
  return parent + "." + key;
}

const nlohmann::json& ReadObject(const nlohmann::json& parent,
                                 const char* key,
                                 const std::string& parent_path) {
  const std::string path = MakeConfigPath(parent_path, key);
  if (!parent.contains(key)) {
    throw std::runtime_error(std::string("Missing config field: ") + path);
  }

  const nlohmann::json& value = parent.at(key);
  if (!value.is_object()) {
    throw std::runtime_error(std::string("Config field must be an object: ") +
                             path);
  }
  return value;
}

std::string ReadString(const nlohmann::json& object,
                       const char* key,
                       const std::string& object_path) {
  const std::string path = MakeConfigPath(object_path, key);
  if (!object.contains(key)) {
    throw std::runtime_error(std::string("Missing config field: ") + path);
  }

  const nlohmann::json& value = object.at(key);
  if (!value.is_string()) {
    throw std::runtime_error(std::string("Config field must be a string: ") +
                             path);
  }

  const std::string result = value.get<std::string>();
  if (result.empty()) {
    throw std::runtime_error(std::string("Config field must not be empty: ") +
                             path);
  }
  return result;
}

int ReadInteger(const nlohmann::json& object,
                const char* key,
                const std::string& object_path,
                int minimum,
                int maximum) {
  const std::string path = MakeConfigPath(object_path, key);
  if (!object.contains(key)) {
    throw std::runtime_error(std::string("Missing config field: ") + path);
  }

  const nlohmann::json& value = object.at(key);
  if (!value.is_number_integer()) {
    throw std::runtime_error(std::string("Config field must be an integer: ") +
                             path);
  }

  const int64_t parsed = value.get<int64_t>();
  if (parsed < minimum || parsed > maximum) {
    throw std::runtime_error(std::string("Config field is out of range: ") +
                             path);
  }
  return static_cast<int>(parsed);
}

nlohmann::json ReadConfigFile(const std::string& config_path) {
  std::ifstream input(config_path);
  if (!input.is_open()) {
    throw std::runtime_error(std::string("Failed to open config file: ") +
                             config_path);
  }

  nlohmann::json root;
  try {
    input >> root;
  } catch (const nlohmann::json::exception& ex) {
    throw std::runtime_error(std::string("Failed to parse config JSON: ") +
                             ex.what());
  }
  if (!root.is_object()) {
    throw std::runtime_error("Config root must be an object");
  }
  return root;
}

std::string ReadOptionValue(int argc,
                            char** argv,
                            int* current_index,
                            const char* option_name) {
  if (current_index == nullptr || *current_index + 1 >= argc) {
    throw std::runtime_error(std::string("Missing value for option: ") +
                             option_name);
  }

  *current_index += 1;
  return argv[*current_index];
}

void PrintUsage(const char* program) {
  std::cout
      << "用法：" << program << " [--config rtc.cfg]\n\n"
      << "边缘端所有运行参数都从 rtc.cfg 的 edge 配置段读取。\n\n"
      << "选项：\n"
      << "  --config rtc.cfg    配置文件，默认使用可执行程序附近的 rtc.cfg\n"
      << "  --help              显示帮助\n"
      << std::endl;
}

std::string ParseConfigPath(int argc, char** argv) {
  std::string config_path = "rtc.cfg";

  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    if (argument == "--help" || argument == "-h") {
      PrintUsage(argv[0]);
      std::exit(0);
    }
    if (argument == "--config") {
      config_path = ReadOptionValue(argc, argv, &i, "--config");
      continue;
    }
    throw std::runtime_error(
        std::string("Unknown option: ") + argument +
        "; put runtime settings in the edge section of rtc.cfg");
  }
  return config_path;
}

}  // 匿名命名空间

EdgeOptions LoadEdgeOptions(const std::string& config_path) {
  const std::string resolved_path =
      rtc_camera_headless::ResolveConfigPath(config_path);
  const nlohmann::json root = ReadConfigFile(resolved_path);
  const nlohmann::json& edge = ReadObject(root, "edge", "");
  const nlohmann::json& rtc = ReadObject(edge, "rtc", "edge");
  const nlohmann::json& camera =
      ReadObject(edge, "stereo_camera", "edge");
  const nlohmann::json& surround_camera =
      ReadObject(edge, "surround_camera", "edge");
  const nlohmann::json& control =
      ReadObject(edge, "vehicle_control", "edge");

  EdgeOptions options;
  options.rtc.config_path = resolved_path;
  options.rtc.room_id = ReadString(rtc, "room", "edge.rtc");
  options.rtc.join_retry_ms = ReadInteger(
      rtc, "join_retry_ms", "edge.rtc", 0,
      std::numeric_limits<int>::max());
  options.rtc.status_interval_sec = ReadInteger(
      rtc, "status_interval_sec", "edge.rtc", 0,
      std::numeric_limits<int>::max());
  options.rtc.frame_limit = ReadInteger(
      rtc, "frame_limit", "edge.rtc", 0,
      std::numeric_limits<int>::max());

  options.camera.capture.left_device =
      ReadString(camera, "left_device", "edge.stereo_camera");
  options.camera.capture.right_device =
      ReadString(camera, "right_device", "edge.stereo_camera");
  options.camera.capture.width = ReadInteger(
      camera, "width", "edge.stereo_camera", 0,
      std::numeric_limits<int>::max());
  options.camera.capture.height = ReadInteger(
      camera, "height", "edge.stereo_camera", 0,
      std::numeric_limits<int>::max());
  options.camera.capture.buffer_count = ReadInteger(
      camera, "buffer_count", "edge.stereo_camera", 2,
      std::numeric_limits<int>::max());
  options.camera.capture.timeout_ms = ReadInteger(
      camera, "timeout_ms", "edge.stereo_camera", 0,
      std::numeric_limits<int>::max());
  options.camera.capture.warmup_frames = ReadInteger(
      camera, "warmup_frames", "edge.stereo_camera", 0,
      std::numeric_limits<int>::max());
  options.camera.capture.warmup_delay_ms = ReadInteger(
      camera, "warmup_delay_ms", "edge.stereo_camera", 0,
      std::numeric_limits<int>::max());
  const int frame_wait_ms = ReadInteger(
      camera, "frame_wait_ms", "edge.stereo_camera", 1,
      std::numeric_limits<int>::max());
  options.camera.frame_wait = std::chrono::milliseconds(frame_wait_ms);

  options.surround_camera.front_device = ReadString(
      surround_camera, "front_device", "edge.surround_camera");
  options.surround_camera.rear_device = ReadString(
      surround_camera, "rear_device", "edge.surround_camera");
  options.surround_camera.left_device = ReadString(
      surround_camera, "left_device", "edge.surround_camera");
  options.surround_camera.right_device = ReadString(
      surround_camera, "right_device", "edge.surround_camera");
  options.surround_camera.width = ReadInteger(
      surround_camera, "width", "edge.surround_camera", 0,
      std::numeric_limits<int>::max());
  options.surround_camera.height = ReadInteger(
      surround_camera, "height", "edge.surround_camera", 0,
      std::numeric_limits<int>::max());
  options.surround_camera.buffer_count = ReadInteger(
      surround_camera, "buffer_count", "edge.surround_camera", 2,
      std::numeric_limits<int>::max());
  options.surround_camera.timeout_ms = ReadInteger(
      surround_camera, "timeout_ms", "edge.surround_camera", 0,
      std::numeric_limits<int>::max());
  options.surround_camera.warmup_frames = ReadInteger(
      surround_camera, "warmup_frames", "edge.surround_camera", 0,
      std::numeric_limits<int>::max());
  options.surround_camera.warmup_delay_ms = ReadInteger(
      surround_camera, "warmup_delay_ms", "edge.surround_camera", 0,
      std::numeric_limits<int>::max());
  const int surround_frame_wait_ms = ReadInteger(
      surround_camera, "frame_wait_ms", "edge.surround_camera", 1,
      std::numeric_limits<int>::max());
  options.surround_camera.frame_wait =
      std::chrono::milliseconds(surround_frame_wait_ms);

  const int watchdog_ms = ReadInteger(
      control, "watchdog_ms", "edge.vehicle_control", 1,
      static_cast<int>(vts_rtc::vehicle::kDefaultDriveWatchdogMs));
  options.control.watchdog_ms = static_cast<uint32_t>(watchdog_ms);
  const int state_interval_ms = ReadInteger(
      control, "state_interval_ms", "edge.vehicle_control", 1,
      std::numeric_limits<int>::max());
  options.control.state_interval_ms =
      static_cast<uint32_t>(state_interval_ms);
  const int max_pending_events = ReadInteger(
      control, "max_pending_events", "edge.vehicle_control", 1,
      std::numeric_limits<int>::max());
  options.control.max_pending_events =
      static_cast<size_t>(max_pending_events);
  return options;
}

EdgeOptions ParseEdgeArgs(int argc, char** argv) {
  return LoadEdgeOptions(ParseConfigPath(argc, argv));
}

}  // 命名空间 rtc_edge_headless
