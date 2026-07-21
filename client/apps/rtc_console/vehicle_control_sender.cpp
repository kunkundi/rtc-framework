#include "vehicle_control_sender.h"

#include <algorithm>

namespace rtc_console {

float ClampVehicleThrottle(float throttle) {
  return std::max(0.0f, std::min(1.0f, throttle));
}

vts_rtc::vehicle::DriveCommand MakeVehicleDriveCommand(
    const VehicleControlInput& input) {
  vts_rtc::vehicle::DriveCommand command;
  if (input.emergency_stop) {
    command.drive_direction = vts_rtc::vehicle::DriveDirection::Stop;
    command.steering_direction = vts_rtc::vehicle::SteeringDirection::Center;
    command.throttle = 0.0f;
    command.brake = 1.0f;
    return command;
  }

  if (input.forward && !input.reverse) {
    command.drive_direction = vts_rtc::vehicle::DriveDirection::Forward;
  } else if (input.reverse && !input.forward) {
    command.drive_direction = vts_rtc::vehicle::DriveDirection::Reverse;
  } else {
    command.drive_direction = vts_rtc::vehicle::DriveDirection::Stop;
  }

  if (input.left && !input.right) {
    command.steering_direction = vts_rtc::vehicle::SteeringDirection::Left;
  } else if (input.right && !input.left) {
    command.steering_direction = vts_rtc::vehicle::SteeringDirection::Right;
  } else {
    command.steering_direction = vts_rtc::vehicle::SteeringDirection::Center;
  }

  command.throttle = ClampVehicleThrottle(input.throttle);
  command.brake = 0.0f;
  return command;
}

}  // namespace rtc_console
