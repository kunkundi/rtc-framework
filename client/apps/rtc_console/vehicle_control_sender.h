#pragma once

#include "rtc_vehicle_protocol/vehicle_control_protocol.h"

namespace rtc_console {

struct VehicleControlInput {
  bool forward = false;
  bool reverse = false;
  bool left = false;
  bool right = false;
  bool emergency_stop = false;
  float throttle = 0.0f;
};

float ClampVehicleThrottle(float throttle);
vts_rtc::vehicle::DriveCommand MakeVehicleDriveCommand(
    const VehicleControlInput& input);

}  // namespace rtc_console
