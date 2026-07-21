#include "vehicle_control_sender.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void Check(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "check failed: " << message << std::endl;
    std::exit(1);
  }
}

void TestForwardLeft() {
  rtc_console::VehicleControlInput input;
  input.forward = true;
  input.left = true;
  input.throttle = 0.5f;

  const vts_rtc::vehicle::DriveCommand command =
      rtc_console::MakeVehicleDriveCommand(input);
  Check(command.drive_direction ==
            vts_rtc::vehicle::DriveDirection::Forward,
        "forward direction");
  Check(command.steering_direction ==
            vts_rtc::vehicle::SteeringDirection::Left,
        "left steering");
  Check(command.throttle == 0.5f, "forward throttle");
  Check(command.brake == 0.0f, "forward brake");
}

void TestConflictingInputsStop() {
  rtc_console::VehicleControlInput input;
  input.forward = true;
  input.reverse = true;
  input.left = true;
  input.right = true;
  input.throttle = 0.7f;

  const vts_rtc::vehicle::DriveCommand command =
      rtc_console::MakeVehicleDriveCommand(input);
  Check(command.drive_direction == vts_rtc::vehicle::DriveDirection::Stop,
        "conflicting drive stops");
  Check(command.steering_direction ==
            vts_rtc::vehicle::SteeringDirection::Center,
        "conflicting steering centers");
}

void TestThrottleClamp() {
  rtc_console::VehicleControlInput input;
  input.forward = true;
  input.throttle = 5.0f;
  Check(rtc_console::MakeVehicleDriveCommand(input).throttle == 1.0f,
        "high throttle clamp");

  input.throttle = -1.0f;
  Check(rtc_console::MakeVehicleDriveCommand(input).throttle == 0.0f,
        "low throttle clamp");
}

void TestEmergencyStop() {
  rtc_console::VehicleControlInput input;
  input.forward = true;
  input.left = true;
  input.emergency_stop = true;
  input.throttle = 0.8f;

  const vts_rtc::vehicle::DriveCommand command =
      rtc_console::MakeVehicleDriveCommand(input);
  Check(command.drive_direction == vts_rtc::vehicle::DriveDirection::Stop,
        "emergency drive stop");
  Check(command.steering_direction ==
            vts_rtc::vehicle::SteeringDirection::Center,
        "emergency steering center");
  Check(command.throttle == 0.0f, "emergency throttle");
  Check(command.brake == 1.0f, "emergency brake");
}

}  // namespace

int main() {
  TestForwardLeft();
  TestConflictingInputsStop();
  TestThrottleClamp();
  TestEmergencyStop();
  std::cout << "rtc_console_vehicle_control_sender_tests passed"
            << std::endl;
  return 0;
}
