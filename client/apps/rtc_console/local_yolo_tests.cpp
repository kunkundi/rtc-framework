#include "local_yolo.h"
#include <atomic>
#include <cstdlib>
#include <iostream>

namespace {
void Check(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << std::endl;
    std::exit(1);
  }
}
}

int main() {
  rtc_console::LocalYolo worker;
  std::string error;
  Check(!worker.Start("missing-model.onnx", 15, {}, &error), "reject invalid callback");
  Check(!worker.Start("missing-model.onnx", 15,
      [](const rtc_console::LocalYoloFrame&, const vts_rtc::vision::FrameDetections&) {},
      &error), "reject missing model");
  std::mutex mutex;
  std::condition_variable ready;
  int results = 0;
  std::string failure;
  Check(worker.Start("", 15,
      [&](const rtc_console::LocalYoloFrame& frame, const vts_rtc::vision::FrameDetections& detections) {
        Check(frame.sequence == detections.frame_id && frame.source == detections.source_id,
              "preserve frame identity");
        Check(detections.frame_width == 64 && detections.frame_height == 48,
              "preserve frame dimensions");
        Check(detections.detections.size() <= 64, "bound detection count");
        std::lock_guard<std::mutex> lock(mutex);
        ++results;
        ready.notify_one();
      }, &error, [&](const std::string& message) {
        std::lock_guard<std::mutex> lock(mutex);
        failure = message;
        ready.notify_one();
      }), error.c_str());
  for (const auto& source : {"stereo", "surround_grid", "surround_front"}) {
    rtc_console::LocalYoloFrame frame;
    frame.key = source;
    frame.source = source;
    frame.sequence = 42;
    frame.width = 64;
    frame.height = 48;
    frame.pixels.assign(64 * 48 * 4, 0);
    frame.received = std::chrono::steady_clock::now();
    worker.Submit(std::move(frame));
  }
  {
    std::unique_lock<std::mutex> lock(mutex);
    Check(ready.wait_for(lock, std::chrono::seconds(30), [&] {
      return results == 3 || !failure.empty();
    }), "inference completes");
    Check(failure.empty(), failure.c_str());
    Check(results == 3, "infer stereo, grid and single-source frames");
  }
  worker.Clear();
  worker.Stop();
  worker.Stop();
  std::cout << "Local YOLO model and lifecycle tests passed" << std::endl;
}