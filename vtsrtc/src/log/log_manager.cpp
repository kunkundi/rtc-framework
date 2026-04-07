#include "log_manager.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <utility>
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

namespace {

constexpr const char* kLoggerName = "rtc_agent_logger";
constexpr const char* kTopic = "rtc_agent";
constexpr const char* kLogFileName = "rtc_agent.log";
constexpr bool kStdoutEnabled = false;
constexpr size_t kMaxLogFileSize = 10 * 1024 * 1024;
constexpr size_t kMaxLogFiles = 10;

std::string FormatMessage(const char* fmt, va_list args) {
  if (!fmt) {
    return std::string();
  }

  va_list args_copy;
  va_copy(args_copy, args);
  const int required = std::vsnprintf(nullptr, 0, fmt, args_copy);
  va_end(args_copy);
  if (required < 0) {
    return std::string();
  }

  std::vector<char> buffer(static_cast<size_t>(required) + 1, '\0');
  std::vsnprintf(buffer.data(), buffer.size(), fmt, args);
  return std::string(buffer.data());
}

std::string NormalizeSeparators(const std::string& path) {
  std::string normalized = path;
  std::replace(normalized.begin(), normalized.end(), '\\', '/');
  return normalized;
}

std::string DirectoryName(const std::string& path) {
  const std::string normalized = NormalizeSeparators(path);
  const std::string::size_type slash = normalized.find_last_of('/');
  if (slash == std::string::npos) {
    return std::string();
  }
  return normalized.substr(0, slash);
}

int MakeDirectory(const char* path) {
#if defined(_WIN32)
  return _mkdir(path);
#else
  return ::mkdir(path, 0755);
#endif
}

bool EnsureDirectoryExists(const std::string& raw_path) {
  if (raw_path.empty() || raw_path == ".") {
    return true;
  }

  const std::string path = NormalizeSeparators(raw_path);
  std::string current;
  std::string::size_type pos = 0;

  if (path.size() >= 2 && path[1] == ':') {
    current = path.substr(0, 2);
    pos = 2;
    if (pos < path.size() && path[pos] == '/') {
      current += "/";
      ++pos;
    }
  } else if (!path.empty() && path[0] == '/') {
    current = "/";
    pos = 1;
  }

  while (pos < path.size()) {
    while (pos < path.size() && path[pos] == '/') {
      ++pos;
    }
    if (pos >= path.size()) {
      break;
    }

    const std::string::size_type next = path.find('/', pos);
    const std::string part =
        next == std::string::npos ? path.substr(pos) : path.substr(pos, next - pos);
    pos = next == std::string::npos ? path.size() : next + 1;

    if (part.empty() || part == ".") {
      continue;
    }

    if (!current.empty() && current.back() != '/') {
      current += "/";
    }
    current += part;

    if (MakeDirectory(current.c_str()) != 0 && errno != EEXIST) {
      return false;
    }
  }

  return true;
}

std::string BuildLogFilePath(const std::string& log_path) {
  if (log_path.empty()) {
    return kLogFileName;
  }

  std::string file_path = log_path;
  if (file_path.back() != '/' && file_path.back() != '\\') {
    file_path += "/";
  }
  file_path += kLogFileName;
  return file_path;
}

std::string BuildPattern() {
  return std::string("[%L][%Y%m%d-%H%M%S.%e][%s:%#][") + kTopic + "]:%v";
}

spdlog::level::level_enum ToSpdlogLevel(LogManager::Level level) {
  switch (level) {
    case LogManager::Level::Info:
      return spdlog::level::info;
    case LogManager::Level::Warn:
      return spdlog::level::warn;
    case LogManager::Level::Error:
      return spdlog::level::err;
    default:
      return spdlog::level::info;
  }
}

}  // namespace

LogManager::LogManager() {}

LogManager::~LogManager() {}

int LogManager::init(std::string log_path) {
  std::lock_guard<std::mutex> lock(mutex_);
  log_path_ = std::move(log_path);
  ResetLoggerLocked();
  return 0;
}

void LogManager::Log(Level level,
                     const char* file,
                     int line,
                     const char* fmt,
                     ...) {
  va_list args;
  va_start(args, fmt);
  VLog(level, file, line, fmt, args);
  va_end(args);
}

void LogManager::VLog(Level level,
                      const char* file,
                      int line,
                      const char* fmt,
                      va_list args) {
  const std::string message = FormatMessage(fmt, args);

  std::lock_guard<std::mutex> lock(mutex_);
  if (!logger_) {
    ResetLoggerLocked();
  }

  logger_->log(spdlog::source_loc{file ? file : "", line, ""},
               ToSpdlogLevel(level), "{}", message);
}

void LogManager::ResetLoggerLocked() {
  std::vector<spdlog::sink_ptr> sinks;
  const std::string pattern = BuildPattern();

  if (kStdoutEnabled) {
    auto stdout_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    stdout_sink->set_level(spdlog::level::info);
    stdout_sink->set_pattern(pattern);
    sinks.push_back(stdout_sink);
  }

  const std::string log_file_path = BuildLogFilePath(log_path_);
  if (EnsureDirectoryExists(DirectoryName(log_file_path))) {
    auto file_sink =
        std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            log_file_path, kMaxLogFileSize, kMaxLogFiles);
    file_sink->set_level(spdlog::level::trace);
    file_sink->set_pattern(pattern);
    sinks.push_back(file_sink);
  }

  logger_ = std::make_shared<spdlog::logger>(kLoggerName, sinks.begin(), sinks.end());
  logger_->set_level(spdlog::level::trace);
  logger_->flush_on(spdlog::level::trace);
}
