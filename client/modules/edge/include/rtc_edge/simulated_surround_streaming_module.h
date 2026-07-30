#pragma once

#include <stddef.h>
#include <stdint.h>

#include <chrono>
#include <fstream>
#include <string>
#include <vector>

namespace rtc_runtime {
class RtcSession;
}

namespace rtc_edge {

struct SimulatedSurroundStreamingModuleOptions {
  std::string yuv_path;
  size_t width = 0;
  size_t height = 0;
  int fps = 30;
};

class SimulatedSurroundStreamingModule {
 public:
  explicit SimulatedSurroundStreamingModule(
      const SimulatedSurroundStreamingModuleOptions& options);
  ~SimulatedSurroundStreamingModule();

  SimulatedSurroundStreamingModule(
      const SimulatedSurroundStreamingModule&) = delete;
  SimulatedSurroundStreamingModule& operator=(
      const SimulatedSurroundStreamingModule&) = delete;

  bool Start(std::string* error_message);
  void Stop();
  bool Tick(rtc_runtime::RtcSession* rtc_session,
            std::string* error_message);

  bool started() const { return started_; }

 private:
  bool ReadNextFrame(std::string* error_message);
  bool SendSimulatedCamera(rtc_runtime::RtcSession* rtc_session,
                           const char* source_id,
                           int transform,
                           std::string* error_message);
  void TransformFrame(int transform);

  SimulatedSurroundStreamingModuleOptions options_;
  std::ifstream input_;
  std::vector<uint8_t> input_frame_;
  std::vector<uint8_t> transformed_frame_;
  std::chrono::steady_clock::duration frame_period_;
  std::chrono::steady_clock::time_point next_frame_time_;
  bool started_ = false;
  bool first_frame_logged_ = false;
};

}  // namespace rtc_edge
