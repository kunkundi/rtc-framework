#include "log_manager.h"

#include <cstdio>
#include <vector>

#include <spdlog/logger.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

namespace {

constexpr const char* kLoggerName = "rtc_signaling_server_logger";
constexpr const char* kTopic = "rtc_signaling_server";
constexpr const char* kLogFileName = "rtc_signaling_server.log";
constexpr bool kStdoutEnabled = true;
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

int LogManager::init() {
  std::lock_guard<std::mutex> lock(mutex_);
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

  auto file_sink =
      std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
          kLogFileName, kMaxLogFileSize, kMaxLogFiles);
  file_sink->set_level(spdlog::level::trace);
  file_sink->set_pattern(pattern);
  sinks.push_back(file_sink);

  logger_ = std::make_shared<spdlog::logger>(kLoggerName, sinks.begin(), sinks.end());
  logger_->set_level(spdlog::level::trace);
  logger_->flush_on(spdlog::level::trace);
}
