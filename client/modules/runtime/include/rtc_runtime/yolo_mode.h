#pragma once

#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

namespace rtc_runtime {

enum class YoloMode { Console, Edge, Disabled };

inline const char* YoloModeName(YoloMode mode) {
  switch (mode) {
    case YoloMode::Console: return "console";
    case YoloMode::Edge: return "edge";
    case YoloMode::Disabled: return "disabled";
  }
  throw std::runtime_error("invalid YOLO mode");
}

// 共享模式优先于旧版两端各自的 enabled；未配置时保留调用方的旧行为。
inline YoloMode ReadYoloMode(const nlohmann::json& root, YoloMode fallback) {
  if (!root.contains("yolo")) return fallback;
  const auto& yolo = root.at("yolo");
  if (!yolo.is_object() || !yolo.contains("mode") ||
      !yolo.at("mode").is_string()) {
    throw std::runtime_error("yolo.mode must be a string: console, edge or disabled");
  }
  const auto mode = yolo.at("mode").get<std::string>();
  if (mode == "console") return YoloMode::Console;
  if (mode == "edge") return YoloMode::Edge;
  if (mode == "disabled") return YoloMode::Disabled;
  throw std::runtime_error("yolo.mode must be console, edge or disabled");
}

}  // 命名空间 rtc_runtime
