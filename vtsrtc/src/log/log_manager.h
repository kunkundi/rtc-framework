#pragma once

#include <cstdarg>
#if defined(VTSRTC_USE_LIBVTSLOG) && VTSRTC_USE_LIBVTSLOG
#include "vtslog.h"
#endif

#include <mutex>
#include <string>

class LogManager {
 public:
  enum class Level {
    Info,
    Warn,
    Error,
  };

  LogManager();
  virtual ~LogManager();

  static LogManager* GetInstance() {
    static LogManager instance;
    return &instance;
  }

  int init(std::string log_path);
  void Log(Level level, const char* file, int line, const char* fmt, ...);

 private:
#if defined(VTSRTC_USE_LIBVTSLOG) && VTSRTC_USE_LIBVTSLOG
  vts::log::LOG_LEVEL ToVtsLevel(Level level) const;
  vts::log::vtslog log_;
  std::string topic_[1] = {"rtc_agent"};
  std::string log_path_;
#else
  void VLog(Level level, const char* file, int line, const char* fmt, va_list args);
  void EnsureLogDirectoryLocked();
  std::string FormatPrefix(Level level, const char* file, int line) const;
  const char* LevelTag(Level level) const;
  std::string BaseName(const char* path) const;

  mutable std::mutex mutex_;
  std::string log_path_;
  bool initialized_ = false;
  bool directory_ready_ = false;
#endif
};

#define LogInst LogManager::GetInstance()

#define LOG_EVERY_N(num, n, fmt, ...) \
  if ((num) % (n) == 0) { \
    LogInst->Log(LogManager::Level::Info, __FILE__, __LINE__, fmt, ##__VA_ARGS__); \
  }

#define LOG_INFO(fmt, ...) \
  LogInst->Log(LogManager::Level::Info, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) \
  LogInst->Log(LogManager::Level::Error, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...) \
  LogInst->Log(LogManager::Level::Warn, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
