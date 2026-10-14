#pragma once

#include <cmath>
#include <cstdint>

namespace vts_rtc {

struct VideoDelayWindow {
  bool initialized = false;
  double total_seconds = 0;
  uint64_t count = 0;

  double Update(double seconds, uint64_t frames) {
    if (!std::isfinite(seconds) || seconds < 0) {
      return -1;
    }
    double result = -1;
    if (initialized && seconds >= total_seconds && frames > count) {
      result = (seconds - total_seconds) * 1000 / (frames - count);
    }
    initialized = true;
    total_seconds = seconds;
    count = frames;
    return result;
  }
};

inline double EstimateVideoDelay(double network_ms, double buffer_ms,
                                 double decode_ms) {
  if (!std::isfinite(network_ms) || !std::isfinite(buffer_ms) ||
      !std::isfinite(decode_ms) || network_ms < 0 || buffer_ms < 0 ||
      decode_ms < 0) {
    return -1;
  }
  return network_ms + buffer_ms + decode_ms;
}

}