#include "rtc_console_options.h"
#include "device_slot_registry.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void Check(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << std::endl;
    std::exit(1);
  }
}

rtc_console::AppOptions Parse(const std::vector<std::string>& arguments) {
  std::vector<char*> argv;
  for (const std::string& argument : arguments) {
    argv.push_back(const_cast<char*>(argument.c_str()));
  }
  return rtc_console::ParseOptions(static_cast<int>(argv.size()), argv.data());
}

void WriteConfig(const std::string& path, const std::string& content) {
  std::ofstream output(path);
  Check(output.is_open(), "open temporary config");
  output << content;
  Check(static_cast<bool>(output), "write temporary config");
}

void TestDefaults() {
  const rtc_console::AppOptions options = Parse({"rtc_console"});
  Check(!options.no_render, "render by default");
  Check(options.room_id == "zhejianglab", "default room");
}

void TestNoRenderAndRoom() {
  const rtc_console::AppOptions options =
      Parse({"rtc_console", "--no-render", "--room", "test-room"});
  Check(options.no_render, "enable no-render mode");
  Check(options.room_id_from_command_line,
        "mark command-line room override");
  Check(options.room_id == "test-room", "parse room");

  const rtc_console::AppOptions alias =
      Parse({"rtc_console", "--headless"});
  Check(alias.no_render, "accept headless alias");
}

void TestConfig() {
  const std::string path = "rtc_console_options_test_config.json";

  WriteConfig(path, "{\"console\":{\"yolo\":{\"enabled\":false,\"max_fps\":12,\"model\":\"models/test.onnx\"}}}");
  const auto yolo_config = rtc_console::LoadConfig(path);
  Check(yolo_config.yolo_mode == rtc_runtime::YoloMode::Edge && yolo_config.yolo_max_fps == 12 &&
        yolo_config.yolo_model_path == "models/test.onnx", "load console YOLO config");
  for (const auto& yolo : {"[]", "{\"enabled\":1}", "{\"max_fps\":0}",
                          "{\"max_fps\":241}", "{\"max_fps\":1.5}",
                          "{\"model\":null}"}) {
    WriteConfig(path, std::string("{\"console\":{\"yolo\":") + yolo + "}}");
    bool rejected = false;
    try { rtc_console::LoadConfig(path); }
    catch (const std::runtime_error&) { rejected = true; }
    Check(rejected, "reject invalid console YOLO config");
  }

  WriteConfig(path,
              "{\"console\":{\"room\":\"configured-room\","
              "\"auto_open_room\":true}}");
  const rtc_console::AppConfig enabled_config = rtc_console::LoadConfig(path);
  Check(enabled_config.room_id == "configured-room",
        "load configured room");
  Check(enabled_config.auto_open_room,
        "load enabled room auto-open");

  WriteConfig(path,
              "{\"console\":{\"room\":\"other-room\","
              "\"auto_open_room\":false}}");
  const rtc_console::AppConfig disabled_config = rtc_console::LoadConfig(path);
  Check(disabled_config.room_id == "other-room",
        "load second configured room");
  Check(!disabled_config.auto_open_room,
        "load disabled room auto-open");

  WriteConfig(path, "{}");
  const rtc_console::AppConfig default_config = rtc_console::LoadConfig(path);
  Check(default_config.room_id == "zhejianglab", "default configured room");
  Check(!default_config.auto_open_room,
        "default room auto-open to disabled");
    Check(default_config.steering_wheel_range_deg == 900.0 &&
      !default_config.steering_servo_calibrated, "default steering is uncalibrated");
    for (const auto& limits : {"\"left_deg\":60,\"right_deg\":120",
               "\"left_deg\":120,\"right_deg\":60"}) {
      WriteConfig(path, std::string("{\"console\":{\"steering\":{\"wheel_range_deg\":1080,") +
      "\"servo\":{\"center_deg\":90," + limits + "}}}}");
      const auto calibrated = rtc_console::LoadConfig(path);
      Check(calibrated.steering_wheel_range_deg == 1080.0 &&
        calibrated.steering_servo_calibrated &&
        calibrated.steering_servo_center_deg == 90.0 &&
        calibrated.steering_servo_left_deg + calibrated.steering_servo_right_deg == 180.0,
        "load steering calibration including reversed installation");
    }
    for (const auto& steering : {"[]", "{\"wheel_range_deg\":0}",
    "{\"wheel_range_deg\":-900}", "{\"wheel_range_deg\":\"900\"}",
    "{\"servo\":null}", "{\"servo\":{\"center_deg\":90}}",
    "{\"servo\":{\"left_deg\":90,\"center_deg\":90,\"right_deg\":120}}",
    "{\"servo\":{\"left_deg\":60,\"center_deg\":130,\"right_deg\":120}}"}) {
      WriteConfig(path, std::string("{\"console\":{\"steering\":") + steering + "}}");
      bool rejected = false;
      try { rtc_console::LoadConfig(path); }
      catch (const std::runtime_error&) { rejected = true; }
      Check(rejected, "reject invalid steering calibration");
    }

  WriteConfig(path, "{\"console\":{\"room\":1}}");
  bool invalid_room_rejected = false;
  try {
    rtc_console::LoadConfig(path);
  } catch (const std::runtime_error& ex) {
    invalid_room_rejected =
        std::string(ex.what()).find("console.room") != std::string::npos;
  }
  Check(invalid_room_rejected, "reject invalid configured room");

  WriteConfig(path, "{\"console\":{\"auto_open_room\":1}}");
  bool invalid_type_rejected = false;
  try {
    rtc_console::LoadConfig(path);
  } catch (const std::runtime_error& ex) {
    invalid_type_rejected =
        std::string(ex.what()).find("console.auto_open_room") !=
        std::string::npos;
  }
  Check(invalid_type_rejected, "reject non-boolean room auto-open");
  std::remove(path.c_str());
}

void TestHelp() {
  const rtc_console::AppOptions options = Parse({"rtc_console", "--help"});
  Check(options.show_help, "parse help");
}

void TestKeyBindings() {
  const std::string path = "rtc_console_keys_test.json";
  WriteConfig(path, "{}");
  const auto defaults = rtc_console::LoadConfig(path);
  Check(defaults.key_bindings.at("forward") == std::vector<std::string>{"UpArrow"},
    "preserve default driving keys");
  Check(defaults.key_bindings.size() == 21, "cover all application keyboard actions");
  WriteConfig(path, "{\"console\":{\"key_bindings\":{\"forward\":[\"W\",\"F1\"],\"takeover\":\"T\"}}}");
  const auto changed = rtc_console::LoadConfig(path);
  Check(changed.key_bindings.at("forward") == std::vector<std::string>({"W", "F1"}) &&
    changed.key_bindings.at("takeover")[0] == "T" &&
    changed.key_bindings.at("brake_takeover")[0] == "B",
    "override selected actions and keep omitted defaults");
  for (const auto& bindings : {"[]", "{\"unknown\":\"W\"}",
   "{\"forward\":\"w\"}", "{\"forward\":\"Ctrl+W\"}",
   "{\"forward\":[]}", "{\"forward\":[1]}", "{\"forward\":null}",
   "{\"forward\":\"B\"}", "{\"forward\":[\"W\",\"W\"]}"}) {
    WriteConfig(path, std::string("{\"console\":{\"key_bindings\":") + bindings + "}}");
    bool rejected = false;
    try { rtc_console::LoadConfig(path); }
    catch (const std::runtime_error&) { rejected = true; }
    Check(rejected, "reject invalid or conflicting keyboard bindings");
  }
  std::remove(path.c_str());
}

void TestExternalConfig() {
  const std::string path = "rtc_console_external_main.json";
  const std::string external = "rtc_console_external.json";
  WriteConfig(external, "{\"room\":\"external-room\",\"key_bindings\":{\"takeover\":\"T\"}}");
  WriteConfig(path, "{\"console\":{\"config\":\"rtc_console_external.json\"},\"edge\":{\"config\":\"missing.json\"}}");
  const auto loaded = rtc_console::LoadConfig("./" + path);
  Check(loaded.room_id == "external-room" && loaded.key_bindings.at("takeover")[0] == "T",
        "load relative external config without loading other role");
  for (const auto& reference : {"{\"config\":\"missing.json\"}",
       "{\"config\":\"\"}", "{\"config\":1}",
       "{\"config\":\"rtc_console_external.json\",\"room\":\"inline\"}"}) {
    WriteConfig(path, std::string("{\"console\":") + reference + "}");
    bool rejected = false;
    try { rtc_console::LoadConfig(path); }
    catch (const std::runtime_error&) { rejected = true; }
    Check(rejected, "reject missing, invalid or mixed external config");
  }
  WriteConfig(path, "{\"console\":{\"config\":\"rtc_console_external.json\"}}");
  for (const auto& content : {"[]", "{", "{\"config\":\"nested.json\"}"}) {
    WriteConfig(external, content);
    bool rejected = false;
    try { rtc_console::LoadConfig(path); }
    catch (const std::runtime_error&) { rejected = true; }
    Check(rejected, "reject invalid or nested external file");
  }
  std::remove(path.c_str());
  std::remove(external.c_str());
}

void TestDeviceSlots() {
  const std::string path = "rtc_console_slots_test.json";
  WriteConfig(path, "{\"console\":{\"device_slots\":[\"car_2\",\"car_1\",\"\",\"car_4\"]}}");
  const auto config = rtc_console::LoadConfig(path);
  rtc_console::DeviceSlotRegistry registry;
  registry.Configure(config.device_slots);
  registry.Identify(20, "car_1");
  registry.Identify(10, "car_2");
  registry.Identify(30, "unbound");
  auto slots = registry.Snapshot();
  Check(slots[0].session_id == 10 && slots[1].session_id == 20,
    "arrival order does not change bindings");
  Check(slots[2].session_id == 0 && slots[3].session_id == 0,
    "unbound peers do not occupy empty slots");
  registry.Disconnect(10);
  Check(registry.Snapshot()[0].session_id == 0 &&
    registry.Snapshot()[1].session_id == 20, "offline slot does not compact");
  registry.Identify(40, "car_2");
  registry.Disconnect(10);
  Check(registry.Snapshot()[0].session_id == 40, "reconnect retains slot");
  registry.Identify(50, "car_2");
  Check(registry.Snapshot()[0].conflict &&
    registry.Snapshot()[0].session_id == 0, "duplicate identity blocks slot");
  registry.Disconnect(50);
  Check(registry.Snapshot()[0].session_id == 40, "conflict clears on disconnect");
  registry.Clear();
  Check(registry.Snapshot()[0].session_id == 0 &&
    registry.Snapshot()[0].device_id == "car_2", "reset preserves bindings");
  for (const auto& content : {
  "{\"console\":{\"device_slots\":[\"car_1\"]}}",
  "{\"console\":{\"device_slots\":[\"car_1\",\"car_1\",\"\",\"\"]}}",
  "{\"console\":{\"device_slots\":[1,\"\",\"\",\"\"]}}",
  "{\"console\":{\"device_slots\":[\"car 1\",\"\",\"\",\"\"]}}"}) {
    WriteConfig(path, content);
    bool rejected = false;
    try { rtc_console::LoadConfig(path); }
    catch (const std::runtime_error&) { rejected = true; }
    Check(rejected, "reject invalid slot configuration");
  }
  std::remove(path.c_str());
}

void TestSaveConfig() {
  const std::string path = "rtc_console_save_test.json";
  const std::string external = "rtc_console_save_external.json";
  WriteConfig(path, "{\"api_server\":\"old:9090\",\"edge\":{\"config\":\"missing.json\"},\"console\":{\"custom\":17}}");
  auto config = rtc_console::LoadConfig(path);
  config.room_id = "saved-room";
  config.yolo_mode = rtc_runtime::YoloMode::Edge;
  config.yolo_max_fps = 24;
  config.yolo_model_path = "models/custom.onnx";
  config.auto_open_room = true;
  config.steering_servo_calibrated = true;
  config.steering_servo_left_deg = 60;
  config.steering_servo_center_deg = 90;
  config.steering_servo_right_deg = 120;
  config.key_bindings["takeover"] = {"T", "F1"};
  config.rtc_settings["api_server"] = "new:9090";
  rtc_console::SaveConfig(path, config);
  const auto saved = rtc_console::LoadConfig(path);
  Check(saved.room_id == "saved-room" && saved.auto_open_room &&
    saved.key_bindings.at("takeover").size() == 2 &&
    saved.steering_servo_calibrated && saved.yolo_max_fps == 24 &&
    saved.yolo_mode == rtc_runtime::YoloMode::Edge && saved.yolo_model_path == config.yolo_model_path,
    "round trip all console settings");
  nlohmann::json root;
  { std::ifstream input(path); input >> root; }
  Check(root["console"]["custom"] == 17 && root["edge"]["config"] == "missing.json" &&
    root["api_server"] == "new:9090", "preserve unrelated fields and save RTC settings");
  config.room_id.clear();
  bool rejected = false;
  try { rtc_console::SaveConfig(path, config); }
  catch (const std::exception&) { rejected = true; }
  Check(rejected && rtc_console::LoadConfig(path).room_id == "saved-room",
    "invalid settings do not overwrite file");
    config.room_id = "saved-room";
    config.rtc_settings["api_server"] = "";
    rejected = false;
    try { rtc_console::SaveConfig(path, config); }
    catch (const std::exception&) { rejected = true; }
    Check(rejected && rtc_console::LoadConfig(path).rtc_settings.at("api_server") == "new:9090",
      "reject empty server without overwriting settings");
  WriteConfig(external, "{\"custom\":42}");
  WriteConfig(path, "{\"console\":{\"config\":\"rtc_console_save_external.json\"},\"api_server\":\"old\"}");
  config = rtc_console::LoadConfig(path);
  config.room_id = "external-saved";
  config.rtc_settings["api_server"] = "new";
  rtc_console::SaveConfig("./" + path, config);
  { std::ifstream input(path); input >> root; }
  Check(root["console"]["config"] == external && root["api_server"] == "new",
    "external save preserves reference and updates shared settings");
  Check(rtc_console::LoadConfig(path).room_id == "external-saved",
    "external settings round trip");
  { std::ifstream input(external); input >> root; }
  Check(root["custom"] == 42 && !root.contains("console"), "preserve external root fields");
  std::remove(path.c_str());
  std::remove(external.c_str());
}

void TestInvalidArguments() {
  bool missing_room_rejected = false;
  try {
    Parse({"rtc_console", "--room"});
  } catch (const std::runtime_error&) {
    missing_room_rejected = true;
  }
  Check(missing_room_rejected, "reject missing room");

  bool unknown_rejected = false;
  try {
    Parse({"rtc_console", "--unknown"});
  } catch (const std::runtime_error&) {
    unknown_rejected = true;
  }
  Check(unknown_rejected, "reject unknown argument");
}

void TestYoloModes() {
  using rtc_runtime::YoloMode;
  const std::string path = "rtc_console_yolo_modes_test.json";
  const std::string external = "rtc_console_yolo_modes_external.json";
  WriteConfig(path, "{}");
  Check(rtc_console::LoadConfig(path).yolo_mode == YoloMode::Console,
        "default to console inference");
  for (const auto mode : {YoloMode::Console, YoloMode::Edge, YoloMode::Disabled}) {
    for (bool enabled : {false, true}) {
      nlohmann::json root = {
          {"yolo", {{"mode", rtc_runtime::YoloModeName(mode)}}},
          {"console", {{"yolo", {{"enabled", enabled}}}}}};
      WriteConfig(path, root.dump());
      Check(rtc_console::LoadConfig(path).yolo_mode == mode,
            "shared mode overrides legacy console switch");
      root.erase("console");
      WriteConfig(path, root.dump());
      Check(rtc_console::LoadConfig(path).yolo_mode == mode,
            "shared mode applies without console section");
    }
    for (bool use_external : {false, true}) {
      nlohmann::json section = {{"yolo", {{"enabled", true}, {"custom", 42}}}};
      WriteConfig(external, section.dump());
      nlohmann::json root = {
          {"yolo", {{"mode", "console"}, {"custom", 17}}},
          {"edge", {{"config", "missing-edge.json"}}},
          {"console", use_external ? nlohmann::json{{"config", external}} : section}};
      WriteConfig(path, root.dump());
      auto config = rtc_console::LoadConfig(path);
      Check(!config.rtc_settings.contains("yolo"), "mode has a single settings entry");
      config.yolo_mode = mode;
      rtc_console::SaveConfig(path, config);
      Check(rtc_console::LoadConfig(path).yolo_mode == mode,
            "round trip every YOLO mode with inline and external settings");
      { std::ifstream input(path); input >> root; }
      Check(root["yolo"]["mode"] == rtc_runtime::YoloModeName(mode) &&
            root["yolo"]["custom"] == 17 && root["edge"]["config"] == "missing-edge.json",
            "persist shared mode and preserve unrelated settings without reading edge file");
      if (use_external) {
        Check(root["console"]["config"] == external, "preserve console reference");
        std::ifstream input(external);
        input >> section;
      } else {
        section = root["console"];
      }
      Check(!section["yolo"].contains("enabled") && section["yolo"]["custom"] == 42,
            "remove superseded console switch without losing custom fields");
    }
  }
  for (const auto& invalid : {"null", "[]", "{}", "{\"mode\":null}",
                              "{\"mode\":true}", "{\"mode\":1}",
                              "{\"mode\":\"\"}", "{\"mode\":\"auto\"}"}) {
    WriteConfig(path, std::string("{\"yolo\":") + invalid + "}");
    bool rejected = false;
    try { rtc_console::LoadConfig(path); }
    catch (const std::runtime_error& error) {
      rejected = std::string(error.what()).find("yolo.mode") != std::string::npos;
    }
    Check(rejected, "reject malformed or unknown shared YOLO mode");
  }
  WriteConfig(path, "{}");
  auto invalid_config = rtc_console::LoadConfig(path);
  invalid_config.yolo_mode = static_cast<YoloMode>(99);
  bool rejected = false;
  try { rtc_console::SaveConfig(path, invalid_config); }
  catch (const std::runtime_error&) { rejected = true; }
  Check(rejected && rtc_console::LoadConfig(path).yolo_mode == YoloMode::Console,
        "invalid mode cannot overwrite settings");
  std::remove(path.c_str());
  std::remove(external.c_str());
}

}

int main() {
  TestDefaults();
  TestNoRenderAndRoom();
  TestHelp();
  TestInvalidArguments();
  TestConfig();
  TestKeyBindings();
  TestExternalConfig();
  TestDeviceSlots();
  TestSaveConfig();
  TestYoloModes();
  std::cout << "rtc_console options tests passed" << std::endl;
  return 0;
}
