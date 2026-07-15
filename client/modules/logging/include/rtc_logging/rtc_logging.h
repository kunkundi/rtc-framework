#pragma once

#include <string>

namespace rtc_logging {

bool InitializeLogging(const std::string& log_path,
                       const std::string& application_name,
                       std::string* error_message);
void FlushLogs();
void LogInfo(const std::string& message);
void LogError(const std::string& message);

}  // 命名空间 rtc_logging
