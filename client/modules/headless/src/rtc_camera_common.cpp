#include "rtc_headless/rtc_camera_common.h"

#include <signal.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iostream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace rtc_camera_headless {
namespace {

std::atomic<bool> g_stop_requested(false);
std::mutex g_log_mutex;

void OnSignal(int) {
  g_stop_requested.store(true);
}

bool ParsePositiveInt(const std::string& text, int* value) {
  char* end = nullptr;
  const long parsed = std::strtol(text.c_str(), &end, 10);
  if (!end || *end != '\0' || parsed < 0 ||
      parsed > std::numeric_limits<int>::max()) {
    return false;
  }
  *value = static_cast<int>(parsed);
  return true;
}

}  // namespace

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
  base_candidates.push_back("test_data");

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

    const std::string candidate_test_data =
        JoinPath(JoinPath(base, "test_data"), target_name);
    if (FileExists(candidate_test_data)) {
      return candidate_test_data;
    }
  }

  return target_name;
}

std::string TimestampNow() {
  const auto now = std::chrono::system_clock::now();
  const std::time_t now_time = std::chrono::system_clock::to_time_t(now);
  std::tm tm_now{};
  localtime_r(&now_time, &tm_now);

  char text[32] = {0};
  std::snprintf(text, sizeof(text), "%02d:%02d:%02d", tm_now.tm_hour,
                tm_now.tm_min, tm_now.tm_sec);
  return text;
}

std::string FourccToString(uint32_t fourcc) {
  char text[5] = {
      static_cast<char>(fourcc & 0xff),
      static_cast<char>((fourcc >> 8) & 0xff),
      static_cast<char>((fourcc >> 16) & 0xff),
      static_cast<char>((fourcc >> 24) & 0xff),
      '\0'};
  for (int i = 0; i < 4; ++i) {
    if (text[i] == '\0' || text[i] < 32 || text[i] > 126) {
      text[i] = '.';
    }
  }
  return std::string(text);
}

void LogInfo(const std::string& message) {
  std::lock_guard<std::mutex> lock(g_log_mutex);
  std::cout << "[" << TimestampNow() << "] [INFO] " << message << std::endl;
}

void LogError(const std::string& message) {
  std::lock_guard<std::mutex> lock(g_log_mutex);
  std::cerr << "[" << TimestampNow() << "] [ERROR] " << message << std::endl;
}

void PrintUsage(const char* program) {
  std::cout
      << "Usage: " << program << " [options]\n"
      << "\n"
  << "This binary is headless by design and auto-plays the bundled zjlabs.yuv file when available.\n"
      << "\n"
      << "Options:\n"
  << "  --device /dev/video0       Camera node to open, or pass a .yuv file path\n"
      << "  --room zhejianglab         Room to auto join after RTC login\n"
      << "  --config rtc.cfg           RTC config path, defaults to nearby rtc.cfg\n"
      << "  --width 1280               Requested capture width\n"
      << "  --height 720               Requested capture height\n"
      << "  --buffer-count 4           Number of mmap capture buffers\n"
      << "  --timeout-ms 2000          Poll timeout while waiting for frames\n"
      << "  --warmup-frames 0          Discard N frames after open\n"
      << "  --warmup-delay-ms 0        Sleep before warmup after open\n"
      << "  --join-retry-ms 3000       Retry interval for auto join\n"
      << "  --status-interval-sec 5    Status log interval, 0 disables periodic logs\n"
      << "  --frame-limit 0            Exit after sending N RTC frames, 0 means run until quit\n"
      << "  --help                     Show this message\n"
      << "\n"
      << "Examples:\n"
      << "  " << program << "\n"
      << "  " << program << " --device /dev/video0\n"
      << "  " << program << " --device /path/to/video.yuv --room zhejianglab\n"
      << std::endl;
}

CaptureOptions ParseArgs(int argc, char** argv) {
  CaptureOptions options;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto require_value = [&](const char* name) -> std::string {
      if (i + 1 >= argc) {
        throw std::runtime_error(std::string("missing value for ") + name);
      }
      ++i;
      return argv[i];
    };

    if (arg == "--help" || arg == "-h") {
      PrintUsage(argv[0]);
      std::exit(0);
    } else if (arg == "--device") {
      options.device = require_value("--device");
    } else if (arg == "--room") {
      options.room_id = require_value("--room");
    } else if (arg == "--config") {
      options.config_path = require_value("--config");
    } else if (arg == "--width") {
      if (!ParsePositiveInt(require_value("--width"), &options.width)) {
        throw std::runtime_error("invalid --width value");
      }
    } else if (arg == "--height") {
      if (!ParsePositiveInt(require_value("--height"), &options.height)) {
        throw std::runtime_error("invalid --height value");
      }
    } else if (arg == "--frame-limit") {
      if (!ParsePositiveInt(require_value("--frame-limit"),
                            &options.frame_limit)) {
        throw std::runtime_error("invalid --frame-limit value");
      }
    } else if (arg == "--buffer-count") {
      if (!ParsePositiveInt(require_value("--buffer-count"),
                            &options.buffer_count) ||
          options.buffer_count < 2) {
        throw std::runtime_error("invalid --buffer-count value");
      }
    } else if (arg == "--timeout-ms") {
      if (!ParsePositiveInt(require_value("--timeout-ms"),
                            &options.timeout_ms)) {
        throw std::runtime_error("invalid --timeout-ms value");
      }
    } else if (arg == "--warmup-frames") {
      if (!ParsePositiveInt(require_value("--warmup-frames"),
                            &options.warmup_frames)) {
        throw std::runtime_error("invalid --warmup-frames value");
      }
    } else if (arg == "--warmup-delay-ms") {
      if (!ParsePositiveInt(require_value("--warmup-delay-ms"),
                            &options.warmup_delay_ms)) {
        throw std::runtime_error("invalid --warmup-delay-ms value");
      }
    } else if (arg == "--join-retry-ms") {
      if (!ParsePositiveInt(require_value("--join-retry-ms"),
                            &options.join_retry_ms)) {
        throw std::runtime_error("invalid --join-retry-ms value");
      }
    } else if (arg == "--status-interval-sec") {
      if (!ParsePositiveInt(require_value("--status-interval-sec"),
                            &options.status_interval_sec)) {
        throw std::runtime_error("invalid --status-interval-sec value");
      }
    } else {
      throw std::runtime_error("unknown argument: " + arg);
    }
  }

  return options;
}

}  // namespace rtc_camera_headless
