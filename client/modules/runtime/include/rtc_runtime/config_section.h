#pragma once

#include <fstream>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

namespace rtc_runtime {

inline void LoadConfigSection(nlohmann::json& root,
                              const std::string& section,
                              const std::string& main_path) {
  if (!root.contains(section) || !root.at(section).is_object() ||
      !root.at(section).contains("config")) {
    return;
  }
  const auto& reference = root.at(section);
  if (reference.size() != 1 || !reference.at("config").is_string() ||
      reference.at("config").get<std::string>().empty()) {
    throw std::runtime_error(section + ".config requires a non-empty path without inline fields");
  }
  std::string path = reference.at("config").get<std::string>();
  const bool absolute = path.front() == '/' || path.front() == '\\' ||
      (path.size() > 1 && path[1] == ':');
  if (!absolute) {
    const auto separator = main_path.find_last_of("/\\");
    if (separator != std::string::npos) {
      path = main_path.substr(0, separator + 1) + path;
    }
  }
  std::ifstream input(path);
  if (!input) throw std::runtime_error("failed to open " + section + " config: " + path);
  nlohmann::json config;
  try {
    input >> config;
  } catch (const nlohmann::json::exception& error) {
    throw std::runtime_error("failed to parse " + section + " config " + path + ": " + error.what());
  }
  if (!config.is_object() || config.contains("config")) {
    throw std::runtime_error(section + " config must be an object without nested references: " + path);
  }
  root[section] = std::move(config);
}

}