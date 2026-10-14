#include "statistics/video_delay.h"

#include <cassert>
#include <limits>

int main() {
  vts_rtc::VideoDelayWindow window;
  assert(window.Update(1, 10) < 0);
  assert(std::abs(window.Update(1.2, 20) - 20) < 0.001);
  assert(window.Update(1.2, 20) < 0);
  assert(window.Update(0, 0) < 0);
  assert(window.Update(0, 10) == 0);
  assert(window.Update(std::numeric_limits<double>::infinity(), 20) < 0);
  assert(std::abs(window.Update(0.1, 20) - 10) < 0.001);
  assert(vts_rtc::EstimateVideoDelay(10, 20, 3) == 33);
  assert(vts_rtc::EstimateVideoDelay(0, 0, 0) == 0);
  assert(vts_rtc::EstimateVideoDelay(-1, 20, 3) < 0);
  assert(vts_rtc::EstimateVideoDelay(10, NAN, 3) < 0);
}