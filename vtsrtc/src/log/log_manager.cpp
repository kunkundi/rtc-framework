#include "log_manager.h"

#include <cstdarg>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <vector>

#if defined(VTSRTC_USE_LIBVTSLOG) && VTSRTC_USE_LIBVTSLOG

namespace {

std::string FormatMessage(const char* fmt, va_list args) {
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

}  // namespace

LogManager::LogManager() {}

LogManager::~LogManager() {
  log_.ClsFile("rtc_agent");
}

int LogManager::init(std::string log_path) {
  log_path_ = std::move(log_path);
  if (!log_path_.empty()) {
    log_.InitLog(true, vts::log::LEVEL_INFO, log_path_);
  } else {
    log_.InitLog(true, vts::log::LEVEL_INFO);
  }
  log_.CrtFile("rtc_agent");
  return 0;
}

void LogManager::Log(Level level,
                     const char* file,
                     int line,
                     const char* fmt,
                     ...) {
  va_list args;
  va_start(args, fmt);
  const std::string message = FormatMessage(fmt, args);
  va_end(args);
  log_.Log(ToVtsLevel(level), file ? file : "", line, 1, topic_, "%s",
           message.c_str());
}

vts::log::LOG_LEVEL LogManager::ToVtsLevel(Level level) const {
  switch (level) {
    case Level::Info:
      return vts::log::LEVEL_INFO;
    case Level::Warn:
      return vts::log::LEVEL_WARN;
    case Level::Error:
      return vts::log::LEVEL_ERROR;
    default:
      return vts::log::LEVEL_INFO;
  }
}

#else

#include <errno.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <sys/types.h>

namespace {

std::string TimestampNow() {
  const auto now = std::chrono::system_clock::now();
  const auto now_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) %
      std::chrono::seconds(1);
  const std::time_t now_time = std::chrono::system_clock::to_time_t(now);
  std::tm tm_now{};
  localtime_r(&now_time, &tm_now);

  std::ostringstream oss;
  oss << std::setfill('0') << std::setw(4) << (tm_now.tm_year + 1900)
      << std::setw(2) << (tm_now.tm_mon + 1) << std::setw(2) << tm_now.tm_mday
      << "-" << std::setw(2) << tm_now.tm_hour << std::setw(2) << tm_now.tm_min
      << std::setw(2) << tm_now.tm_sec << "." << std::setw(3)
      << now_ms.count();
  return oss.str();
}

bool EnsureDirectoryExists(const std::string& path) {
  if (path.empty() || path == ".") {
    return true;
  }

  std::string current;
  if (!path.empty() && path[0] == '/') {
    current = "/";
  }

  size_t pos = 0;
  while (pos < path.size()) {
    const size_t next = path.find('/', pos);
    const size_t length =
        next == std::string::npos ? path.size() - pos : next - pos;
    const std::string component = path.substr(pos, length);
    pos = next == std::string::npos ? path.size() : next + 1;

    if (component.empty() || component == ".") {
      continue;
    }

    if (!current.empty() && current.back() != '/') {
      current += "/";
    }
    current += component;

    if (::mkdir(current.c_str(), 0755) != 0 && errno != EEXIST) {
      return false;
    }
  }
  return true;
}

}  // namespace

LogManager::LogManager() {}

LogManager::~LogManager() {}

int LogManager::init(std::string log_path) {
  std::lock_guard<std::mutex> lock(mutex_);
  log_path_ = std::move(log_path);
  initialized_ = true;
  directory_ready_ = false;
  EnsureLogDirectoryLocked();
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
  std::lock_guard<std::mutex> lock(mutex_);
  if (!initialized_) {
    initialized_ = true;
  }
  EnsureLogDirectoryLocked();

  va_list args_copy;
  va_copy(args_copy, args);
  const int required = std::vsnprintf(nullptr, 0, fmt, args_copy);
  va_end(args_copy);

  if (required < 0) {
    return;
  }

  std::vector<char> buffer(static_cast<size_t>(required) + 1, '\0');
  std::vsnprintf(buffer.data(), buffer.size(), fmt, args);

  const std::string prefix = FormatPrefix(level, file, line);
  const std::string message(buffer.data());
  const std::string line_text = prefix + message + "\n";

  std::ostream& stream =
      level == Level::Error ? std::cerr : std::cout;
  stream << line_text;
  stream.flush();

  if (!log_path_.empty() && directory_ready_) {
    const std::string log_file = log_path_ + "/rtc_agent.log";
    FILE* file_handle = std::fopen(log_file.c_str(), "a");
    if (file_handle) {
      std::fwrite(line_text.data(), 1, line_text.size(), file_handle);
      std::fflush(file_handle);
      std::fclose(file_handle);
    }
  }
}

void LogManager::EnsureLogDirectoryLocked() {
  if (directory_ready_ || log_path_.empty()) {
    return;
  }
  directory_ready_ = EnsureDirectoryExists(log_path_);
}

std::string LogManager::FormatPrefix(Level level,
                                     const char* file,
                                     int line) const {
  std::ostringstream oss;
  oss << "[" << LevelTag(level) << "][" << TimestampNow() << "]["
      << BaseName(file) << ":" << line << "][rtc_agent]:";
  return oss.str();
}

const char* LogManager::LevelTag(Level level) const {
  switch (level) {
    case Level::Info:
      return "I";
    case Level::Warn:
      return "W";
    case Level::Error:
      return "E";
    default:
      return "?";
  }
}

std::string LogManager::BaseName(const char* path) const {
  if (!path) {
    return "";
  }
  const char* slash = std::strrchr(path, '/');
  return slash ? std::string(slash + 1) : std::string(path);
}

#endif
