#pragma once

#include <string>

namespace rtc_runtime {

void InstallSignalHandlers();
bool StopRequested();
void RequestStop();

std::string JoinPath(const std::string& base, const std::string& leaf);
bool FileExists(const std::string& path);
std::string ExecutableDir();
std::string ResolveConfigPath(const std::string& requested_path_or_name);

}  // 命名空间 rtc_runtime
