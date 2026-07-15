#include "rtc_logging/rtc_logging.h"

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace {

void Check(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << std::endl;
    std::exit(1);
  }
}

std::string ReadFile(const std::string& path) {
  std::ifstream input(path.c_str(), std::ios::binary);
  Check(input.is_open(), "open log file");

  std::ostringstream content;
  content << input.rdbuf();
  return content.str();
}

void TestFileLogging() {
  const std::string log_directory = "/tmp/rtc_logging_tests";
  const std::string log_file = log_directory + "/test_logging.log";
  std::remove(log_file.c_str());

  std::string error_message;
  Check(rtc_logging::InitializeLogging(
            log_directory, "test_logging", &error_message),
        "initialize spdlog logger: " + error_message);

  rtc_logging::LogInfo("external info message");
  rtc_logging::LogError("external error message");
  rtc_logging::FlushLogs();

  const std::string content = ReadFile(log_file);
  Check(content.find("[info] [test_logging] external info message") !=
            std::string::npos,
        "write info message");
  Check(content.find("[error] [test_logging] external error message") !=
            std::string::npos,
        "write error message");

  std::remove(log_file.c_str());
  rmdir(log_directory.c_str());
}

}  // 匿名命名空间

int main() {
  TestFileLogging();
  std::cout << "rtc_logging_tests passed" << std::endl;
  return 0;
}
