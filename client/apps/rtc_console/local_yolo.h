#pragma once

#include "rtc_vision/vision_detection_codec.h"
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace rtc_console {

struct LocalYoloFrame {
  std::string key;
  std::string source;
  uint64_t sequence = 0;
  int width = 0;
  int height = 0;
  std::vector<uint8_t> pixels;
  std::chrono::steady_clock::time_point received;
};

class LocalYolo {
 public:
  using Result = std::function<void(const LocalYoloFrame&,
                                    const vts_rtc::vision::FrameDetections&)>;
  ~LocalYolo();
  bool Start(const std::string& model, int max_fps, Result result,
             std::string* error, std::function<void(const std::string&)> failure = {});
  void Submit(LocalYoloFrame frame);
  void Clear();
  void Stop();

 private:
  std::mutex mutex_;
  std::condition_variable ready_;
  std::map<std::string, LocalYoloFrame> pending_;
  uint64_t generation_ = 0;
  bool stopped_ = true;
  std::thread worker_;
};

}