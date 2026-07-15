#include "edge_options.h"

#include <stddef.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

namespace {

void Check(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << std::endl;
    std::exit(1);
  }
}

bool IsAsciiText(const std::string& text) {
  for (size_t i = 0; i < text.size(); ++i) {
    if (static_cast<unsigned char>(text[i]) > 127) {
      return false;
    }
  }
  return true;
}

nlohmann::json MakeValidConfig() {
  nlohmann::json root;
  root["log_path"] = "/tmp/rtc_edge_logs";
  root["edge"]["rtc"]["room"] = "test-room";
  root["edge"]["rtc"]["join_retry_ms"] = 1200;
  root["edge"]["rtc"]["status_interval_sec"] = 2;
  root["edge"]["rtc"]["frame_limit"] = 25;

  root["edge"]["stereo_camera"]["left_device"] = "/dev/video4";
  root["edge"]["stereo_camera"]["right_device"] = "/dev/video5";
  root["edge"]["stereo_camera"]["width"] = 1280;
  root["edge"]["stereo_camera"]["height"] = 720;
  root["edge"]["stereo_camera"]["buffer_count"] = 6;
  root["edge"]["stereo_camera"]["timeout_ms"] = 1000;
  root["edge"]["stereo_camera"]["warmup_frames"] = 3;
  root["edge"]["stereo_camera"]["warmup_delay_ms"] = 100;
  root["edge"]["stereo_camera"]["frame_wait_ms"] = 15;

  root["edge"]["surround_camera"]["front_device"] = "/dev/video6";
  root["edge"]["surround_camera"]["rear_device"] = "/dev/video7";
  root["edge"]["surround_camera"]["left_device"] = "/dev/video8";
  root["edge"]["surround_camera"]["right_device"] = "/dev/video9";
  root["edge"]["surround_camera"]["width"] = 1920;
  root["edge"]["surround_camera"]["height"] = 1080;
  root["edge"]["surround_camera"]["buffer_count"] = 8;
  root["edge"]["surround_camera"]["timeout_ms"] = 1500;
  root["edge"]["surround_camera"]["warmup_frames"] = 2;
  root["edge"]["surround_camera"]["warmup_delay_ms"] = 50;
  root["edge"]["surround_camera"]["frame_wait_ms"] = 10;

  root["edge"]["yolo"]["enabled"] = true;
  root["edge"]["yolo"]["processing_downscale"] = 4;

  root["edge"]["vehicle_control"]["watchdog_ms"] = 250;
  root["edge"]["vehicle_control"]["state_interval_ms"] = 40;
  root["edge"]["vehicle_control"]["max_pending_events"] = 64;
  return root;
}

void WriteConfig(const std::string& path, const nlohmann::json& config) {
  std::ofstream output(path);
  Check(output.is_open(), "open temporary config");
  output << config.dump(2);
  Check(static_cast<bool>(output), "write temporary config");
}

void TestValidConfig() {
  const std::string path = "/tmp/rtc_edge_options_valid.json";
  WriteConfig(path, MakeValidConfig());

  const rtc_edge_headless::EdgeOptions options =
      rtc_edge_headless::LoadEdgeOptions(path);
  Check(options.log_path == "/tmp/rtc_edge_logs", "load log path");
  Check(options.rtc.room_id == "test-room", "load room");
  Check(options.rtc.join_retry_ms == 1200, "load join retry");
  Check(options.rtc.frame_limit == 25, "load frame limit");
  Check(options.camera.capture.left_device == "/dev/video4",
        "load left camera");
  Check(options.camera.capture.buffer_count == 6, "load buffer count");
  Check(options.camera.frame_wait.count() == 15, "load frame wait");
  Check(options.surround_camera.front_device == "/dev/video6",
        "load surround front camera");
  Check(options.surround_camera.right_device == "/dev/video9",
        "load surround right camera");
  Check(options.surround_camera.width == 1920,
        "load surround camera width");
  Check(options.surround_camera.frame_wait.count() == 10,
        "load surround frame wait");
  Check(options.camera.yolo_enabled, "load YOLO enabled");
  Check(options.camera.yolo_processing_downscale == 4,
        "load YOLO processing downscale");
  Check(options.control.watchdog_ms == 250, "load watchdog");
  Check(options.control.max_pending_events == 64,
        "load pending event limit");
  std::remove(path.c_str());
}

void TestRepositoryConfig() {
  const rtc_edge_headless::EdgeOptions options =
      rtc_edge_headless::LoadEdgeOptions("rtc.cfg");
  Check(options.log_path == "logs", "load repository log path");
  Check(!options.rtc.room_id.empty(), "load repository room");
  Check(options.camera.capture.left_device == "/dev/video0",
        "load repository left camera");
  Check(options.camera.capture.width == 1280,
        "load repository camera width");
  Check(options.surround_camera.front_device.empty(),
        "load disabled repository surround front camera");
  Check(options.surround_camera.right_device.empty(),
        "load disabled repository surround right camera");
  Check(!options.camera.yolo_enabled, "load repository YOLO disabled");
  Check(options.camera.yolo_processing_downscale == 1,
        "load repository YOLO processing downscale");
  Check(options.control.watchdog_ms == 300,
        "load repository watchdog");
}

void TestInvalidWatchdog() {
  const std::string path = "/tmp/rtc_edge_options_watchdog.json";
  nlohmann::json config = MakeValidConfig();
  config["edge"]["vehicle_control"]["watchdog_ms"] = 301;
  WriteConfig(path, config);

  bool rejected = false;
  try {
    rtc_edge_headless::LoadEdgeOptions(path);
  } catch (const std::runtime_error& ex) {
    const std::string error_message = ex.what();
    rejected = error_message.find("edge.vehicle_control.watchdog_ms") !=
                   std::string::npos &&
               IsAsciiText(error_message);
  }
  Check(rejected, "reject watchdog above safety limit");
  std::remove(path.c_str());
}

void TestMissingCameraDevice() {
  const std::string path = "/tmp/rtc_edge_options_missing.json";
  nlohmann::json config = MakeValidConfig();
  config["edge"]["stereo_camera"].erase("right_device");
  WriteConfig(path, config);

  bool rejected = false;
  try {
    rtc_edge_headless::LoadEdgeOptions(path);
  } catch (const std::runtime_error& ex) {
    const std::string error_message = ex.what();
    rejected = error_message.find("edge.stereo_camera.right_device") !=
                   std::string::npos &&
               IsAsciiText(error_message);
  }
  Check(rejected, "reject missing right camera device");
  std::remove(path.c_str());
}

void TestMissingSurroundCameraDevice() {
  const std::string path = "/tmp/rtc_edge_options_surround_missing.json";
  nlohmann::json config = MakeValidConfig();
  config["edge"]["surround_camera"].erase("rear_device");
  WriteConfig(path, config);

  bool rejected = false;
  try {
    rtc_edge_headless::LoadEdgeOptions(path);
  } catch (const std::runtime_error& ex) {
    const std::string error_message = ex.what();
    rejected = error_message.find("edge.surround_camera.rear_device") !=
                   std::string::npos &&
               IsAsciiText(error_message);
  }
  Check(rejected, "reject missing surround rear camera device");
  std::remove(path.c_str());
}

void TestEmptySurroundCameraDevice() {
  const std::string path = "/tmp/rtc_edge_options_surround_empty.json";
  nlohmann::json config = MakeValidConfig();
  config["edge"]["surround_camera"]["front_device"] = "";
  WriteConfig(path, config);

  const rtc_edge_headless::EdgeOptions options =
      rtc_edge_headless::LoadEdgeOptions(path);
  Check(options.surround_camera.front_device.empty(),
        "allow empty surround camera device");
  Check(options.surround_camera.rear_device == "/dev/video7",
        "keep other surround camera enabled");
  std::remove(path.c_str());
}

void TestInvalidYoloDownscale() {
  const std::string path = "/tmp/rtc_edge_options_yolo_downscale.json";
  nlohmann::json config = MakeValidConfig();
  config["edge"]["yolo"]["processing_downscale"] = 9;
  WriteConfig(path, config);

  bool rejected = false;
  try {
    rtc_edge_headless::LoadEdgeOptions(path);
  } catch (const std::runtime_error& ex) {
    const std::string error_message = ex.what();
    rejected = error_message.find("edge.yolo.processing_downscale") !=
                   std::string::npos &&
               IsAsciiText(error_message);
  }
  Check(rejected, "reject invalid YOLO processing downscale");
  std::remove(path.c_str());
}

}  // 匿名命名空间

int main() {
  TestValidConfig();
  TestRepositoryConfig();
  TestInvalidWatchdog();
  TestMissingCameraDevice();
  TestMissingSurroundCameraDevice();
  TestEmptySurroundCameraDevice();
  TestInvalidYoloDownscale();
  std::cout << "rtc_edge_options_tests passed" << std::endl;
  return 0;
}
