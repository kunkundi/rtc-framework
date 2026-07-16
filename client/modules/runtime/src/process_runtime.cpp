#include "rtc_runtime/process_runtime.h"

#include <signal.h>
#include <unistd.h>

#include <atomic>
#include <fstream>
#include <vector>

namespace rtc_runtime {
namespace {

std::atomic<bool> g_stop_requested(false);

void OnSignal(int) {
  g_stop_requested.store(true);
}

}  // 匿名命名空间

void InstallSignalHandlers() {
  signal(SIGINT, OnSignal);
  signal(SIGTERM, OnSignal);
}

bool StopRequested() {
  return g_stop_requested.load();
}

void RequestStop() {
  g_stop_requested.store(true);
}

std::string JoinPath(const std::string& base, const std::string& leaf) {
  if (base.empty()) {
    return leaf;
  }
  if (base.back() == '/') {
    return base + leaf;
  }
  return base + "/" + leaf;
}

bool FileExists(const std::string& path) {
  std::ifstream file(path.c_str(), std::ios::binary);
  return file.good();
}

std::string ExecutableDir() {
  char path[4096] = {0};
  const ssize_t len = readlink("/proc/self/exe", path, sizeof(path) - 1);
  if (len <= 0) {
    return ".";
  }
  path[len] = '\0';
  std::string exe_path(path);
  const size_t pos = exe_path.find_last_of('/');
  if (pos == std::string::npos) {
    return ".";
  }
  return exe_path.substr(0, pos);
}

std::string ResolveConfigPath(const std::string& requested_path_or_name) {
  if (!requested_path_or_name.empty() && FileExists(requested_path_or_name)) {
    return requested_path_or_name;
  }

  const std::string exe_dir = ExecutableDir();
  std::vector<std::string> base_candidates;
  base_candidates.push_back(".");

  std::string current = exe_dir;
  for (int i = 0; i < 5; ++i) {
    base_candidates.push_back(current);
    current = JoinPath(current, "..");
  }

  const std::string target_name =
      requested_path_or_name.empty() ? "rtc.cfg" : requested_path_or_name;
  for (const std::string& base : base_candidates) {
    if (base.empty()) {
      continue;
    }

    const std::string candidate_direct = JoinPath(base, target_name);
    if (FileExists(candidate_direct)) {
      return candidate_direct;
    }

    const std::string candidate_config =
        JoinPath(JoinPath(base, "config"), target_name);
    if (FileExists(candidate_config)) {
      return candidate_config;
    }
  }

  return target_name;
}

}  // 命名空间 rtc_runtime
