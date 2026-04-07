#pragma once

#include <cstdarg>
#include <memory>
#include <mutex>
#include <spdlog/fwd.h>
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
  void VLog(Level level, const char* file, int line, const char* fmt, va_list args);
  void ResetLoggerLocked();

  std::shared_ptr<spdlog::logger> logger_;
  std::mutex mutex_;
  std::string log_path_;
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
