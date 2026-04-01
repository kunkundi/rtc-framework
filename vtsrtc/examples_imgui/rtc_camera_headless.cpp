#include "rtc_camera_common.h"
#include "rtc_headless_session.h"
#include "uyvy_to_i420_cuda.h"
#include "uyvy_v4l2_camera.h"

#include <stdexcept>
#include <string>
#include <vector>

using namespace rtc_camera_headless;

int main(int argc, char** argv) {
  InstallSignalHandlers();

  try {
    const CaptureOptions options = ParseArgs(argc, argv);

    UyvyV4l2CaptureDevice capture_device;
    capture_device.Open(options);
    LogInfo(std::string("camera: ") + capture_device.device_path() + " " +
            std::to_string(capture_device.width()) + "x" +
            std::to_string(capture_device.height()) + " " +
            FourccToString(capture_device.pixel_format()));

    UyvyToI420CudaConverter converter;
    std::string cuda_error;
    if (!converter.Init(capture_device.width(), capture_device.height(),
                        capture_device.bytes_per_line(), &cuda_error)) {
      throw std::runtime_error("failed to init CUDA UYVY converter: " +
                               cuda_error);
    }
    LogInfo("using CUDA UYVY->I420 converter");

    RunWarmup(capture_device, options);

    RtcHeadlessSession rtc_session(options);
    if (!rtc_session.Init()) {
      return 1;
    }

    std::vector<uint8_t> raw_frame;
    size_t raw_bytes_used = 0;
    bool first_frame_logged = false;

    while (!StopRequested()) {
      rtc_session.Tick();

      if (!capture_device.DequeueRawFrame(&raw_frame, &raw_bytes_used)) {
        continue;
      }

      rtc_session.NoteCapturedFrame();
      if (!first_frame_logged) {
        first_frame_logged = true;
        LogInfo("first camera frame captured");
      }

      if (!rtc_session.IsRoomJoined()) {
        continue;
      }

      const uint8_t* i420_data = nullptr;
      size_t i420_size = 0;
      if (!converter.Convert(raw_frame.data(), raw_bytes_used, &i420_data,
                             &i420_size, &cuda_error)) {
        throw std::runtime_error("failed to convert UYVY to I420 on CUDA: " +
                                 cuda_error);
      }

      rtc_session.SendI420Frame(
          i420_data, i420_size, capture_device.width(), capture_device.height(),
          converter.y_stride(), converter.u_stride(), converter.v_stride());

      if (options.frame_limit > 0 &&
          rtc_session.sent_frames() >= static_cast<uint64_t>(options.frame_limit)) {
        LogInfo("frame limit reached");
        RequestStop();
        break;
      }
    }

    return 0;
  } catch (const std::exception& ex) {
    LogError(std::string("rtc_camera_headless failed: ") + ex.what());
    return 1;
  }
}
