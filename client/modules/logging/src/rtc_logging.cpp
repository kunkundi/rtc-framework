#include "rtc_logging/rtc_logging.h"

#include <algorithm>
#include <cerrno>
#include <memory>
#include <mutex>
#include <vector>

#include <spdlog/logger.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#if defined(_WIN32)
#include <direct.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif

namespace rtc_logging {
namespace {

const size_t kMaxLogFileSize = 10 * 1024 * 1024;
const size_t kMaxLogFileCount = 10;
const char* kLogPattern = "[%Y-%m-%d %H:%M:%S.%e] [%l] [%n] %v";

std::mutex g_log_mutex;
std::shared_ptr<spdlog::logger> g_logger;

int MakeDirectory(const std::string& path) {
#if defined(_WIN32)
  return _mkdir(path.c_str());
#else
  return mkdir(path.c_str(), 0755);
#endif
}

std::string NormalizePath(const std::string& path) {
  std::string normalized = path;
  std::replace(normalized.begin(), normalized.end(), '\\', '/');
  return normalized;
}

bool EnsureDirectoryExists(const std::string& raw_path) {
  if (raw_path.empty() || raw_path == ".") {
    return true;
  }

  const std::string path = NormalizePath(raw_path);
  std::string current;
  size_t position = 0;

  if (path.size() >= 2 && path[1] == ':') {
    current = path.substr(0, 2);
    position = 2;
    if (position < path.size() && path[position] == '/') {
      current += "/";
      ++position;
    }
  } else if (path[0] == '/') {
    current = "/";
    position = 1;
  }

  while (position < path.size()) {
    while (position < path.size() && path[position] == '/') {
      ++position;
    }
    if (position >= path.size()) {
      break;
    }

    const size_t separator = path.find('/', position);
    const std::string part = separator == std::string::npos
                                 ? path.substr(position)
                                 : path.substr(position, separator - position);
    position = separator == std::string::npos ? path.size() : separator + 1;
    if (part.empty() || part == ".") {
      continue;
    }

    if (!current.empty() && current.back() != '/') {
      current += "/";
    }
    current += part;
    if (MakeDirectory(current) != 0 && errno != EEXIST) {
      return false;
    }
  }
  return true;
}

std::string JoinPath(const std::string& directory,
                     const std::string& file_name) {
  if (directory.empty()) {
    return file_name;
  }
  if (directory.back() == '/' || directory.back() == '\\') {
    return directory + file_name;
  }
  return directory + "/" + file_name;
}

std::shared_ptr<spdlog::logger> CreateConsoleLogger(
    const std::string& logger_name) {
  std::shared_ptr<spdlog::sinks::stdout_color_sink_mt> console_sink =
      std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
  console_sink->set_pattern(kLogPattern);

  std::shared_ptr<spdlog::logger> logger =
      std::make_shared<spdlog::logger>(logger_name, console_sink);
  logger->set_level(spdlog::level::info);
  logger->flush_on(spdlog::level::info);
  return logger;
}

std::shared_ptr<spdlog::logger> GetLogger() {
  std::lock_guard<std::mutex> lock(g_log_mutex);
  if (!g_logger) {
    g_logger = CreateConsoleLogger("rtc_external");
  }
  return g_logger;
}

}  // 匿名命名空间

bool InitializeLogging(const std::string& log_path,
                       const std::string& application_name,
                       std::string* error_message) {
  if (application_name.empty()) {
    if (error_message != nullptr) {
      *error_message = "Application name must not be empty";
    }
    return false;
  }

  if (!EnsureDirectoryExists(log_path)) {
    if (error_message != nullptr) {
      *error_message = "Failed to create log directory: " + log_path;
    }
    return false;
  }

  try {
    std::vector<spdlog::sink_ptr> sinks;

    std::shared_ptr<spdlog::sinks::stdout_color_sink_mt> console_sink =
        std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    console_sink->set_pattern(kLogPattern);
    sinks.push_back(console_sink);

    const std::string log_file =
        JoinPath(log_path, application_name + ".log");
    std::shared_ptr<spdlog::sinks::rotating_file_sink_mt> file_sink =
        std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            log_file, kMaxLogFileSize, kMaxLogFileCount);
    file_sink->set_pattern(kLogPattern);
    sinks.push_back(file_sink);

    std::shared_ptr<spdlog::logger> logger =
        std::make_shared<spdlog::logger>(application_name, sinks.begin(),
                                        sinks.end());
    logger->set_level(spdlog::level::info);
    logger->flush_on(spdlog::level::info);

    std::lock_guard<std::mutex> lock(g_log_mutex);
    g_logger = logger;
  } catch (const spdlog::spdlog_ex& ex) {
    if (error_message != nullptr) {
      *error_message = std::string("Failed to create spdlog logger: ") +
                       ex.what();
    }
    return false;
  }

  if (error_message != nullptr) {
    error_message->clear();
  }
  return true;
}

void FlushLogs() {
  GetLogger()->flush();
}

void LogInfo(const std::string& message) {
  GetLogger()->info("{}", message);
}

void LogError(const std::string& message) {
  GetLogger()->error("{}", message);
}

}  // 命名空间 rtc_logging
