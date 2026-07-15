#include "edge_application.h"
#include "edge_options.h"

#include "rtc_headless/rtc_camera_common.h"

#include <exception>
#include <string>

int main(int argc, char** argv) {
  rtc_camera_headless::InstallSignalHandlers();

  try {
    const rtc_edge_headless::EdgeOptions options =
        rtc_edge_headless::ParseEdgeArgs(argc, argv);
    return rtc_edge_headless::RunEdgeApplication(options);
  } catch (const std::exception& ex) {
    rtc_camera_headless::LogError(
        std::string("rtc_edge_headless failed: ") + ex.what());
    return 1;
  }
}
