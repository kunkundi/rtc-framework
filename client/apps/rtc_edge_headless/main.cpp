#include "edge_application.h"
#include "edge_options.h"

#include "rtc_headless/rtc_camera_common.h"
#include "rtc_logging/rtc_logging.h"

#include <exception>
#include <string>

int main(int argc, char** argv) {
  rtc_camera_headless::InstallSignalHandlers();

  try {
    const rtc_edge_headless::EdgeOptions options =
        rtc_edge_headless::ParseEdgeArgs(argc, argv);

    std::string logging_error;
    if (!rtc_logging::InitializeLogging(
            options.log_path, "rtc_edge", &logging_error)) {
      rtc_logging::LogError(
          std::string("Failed to initialize application logging: ") +
          logging_error);
      return 1;
    }

    rtc_logging::LogInfo(
        std::string("Application log directory: ") + options.log_path);
    return rtc_edge_headless::RunEdgeApplication(options);
  } catch (const std::exception& ex) {
    rtc_logging::LogError(
        std::string("rtc_edge_headless failed: ") + ex.what());
    return 1;
  }
}
