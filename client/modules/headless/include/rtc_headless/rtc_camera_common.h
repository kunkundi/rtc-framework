#pragma once

#include <string>

namespace rtc_camera_headless {

struct CaptureOptions {
  std::string device;
  std::string room_id = "zhejianglab";
  std::string config_path;
  int width = 0;
  int height = 0;
  int frame_limit = 0;
  int buffer_count = 4;
  int timeout_ms = 2000;
  int warmup_frames = 0;
  int warmup_delay_ms = 0;
  int join_retry_ms = 3000;
  int status_interval_sec = 5;
};

void InstallSignalHandlers();
bool StopRequested();
void RequestStop();

std::string JoinPath(const std::string& base, const std::string& leaf);
bool FileExists(const std::string& path);
std::string ExecutableDir();
std::string ResolveConfigPath(const std::string& requested_path_or_name);
std::string TimestampNow();

void PrintUsage(const char* program);
CaptureOptions ParseArgs(int argc, char** argv);

}  // namespace rtc_camera_headless
