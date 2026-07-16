#include "vehicle_state_log_limiter.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void Check(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << std::endl;
    std::exit(1);
  }
}

void TestPeriodicLogging() {
  rtc_console::VehicleStateLogLimiter limiter(std::chrono::milliseconds(1000));
  const auto start = rtc_console::VehicleStateLogLimiter::Clock::time_point{};

  Check(limiter.ShouldLog(7, 0, false, start), "log first state");
  Check(!limiter.ShouldLog(7, 0, false, start + std::chrono::milliseconds(999)),
        "suppress unchanged state within interval");
  Check(limiter.ShouldLog(7, 0, false, start + std::chrono::milliseconds(1000)),
        "log unchanged state after interval");
}

void TestSafetyChangesLogImmediately() {
  rtc_console::VehicleStateLogLimiter limiter(std::chrono::milliseconds(1000));
  const auto start = rtc_console::VehicleStateLogLimiter::Clock::time_point{};

  Check(limiter.ShouldLog(8, 0, false, start), "log initial safety state");
  Check(limiter.ShouldLog(8, 1, false, start + std::chrono::milliseconds(10)),
        "log gear change immediately");
  Check(limiter.ShouldLog(8, 1, true, start + std::chrono::milliseconds(20)),
        "log watchdog change immediately");
}

void TestSessionsAreIndependent() {
  rtc_console::VehicleStateLogLimiter limiter(std::chrono::milliseconds(1000));
  const auto start = rtc_console::VehicleStateLogLimiter::Clock::time_point{};

  Check(limiter.ShouldLog(9, 0, false, start), "log first session");
  Check(limiter.ShouldLog(10, 0, false, start), "log second session");
}

}  // 匿名命名空间

int main() {
  TestPeriodicLogging();
  TestSafetyChangesLogImmediately();
  TestSessionsAreIndependent();
  std::cout << "rtc_console vehicle state log limiter tests passed"
            << std::endl;
  return 0;
}
