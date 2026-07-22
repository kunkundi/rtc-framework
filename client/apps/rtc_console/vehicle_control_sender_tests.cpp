#include "vehicle_control_sender.h"

#include <cstdlib>
#include <iostream>
#include <limits>
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

  input.throttle = std::numeric_limits<float>::quiet_NaN();
  Check(rtc_console::MakeVehicleDriveCommand(input).throttle == 0.0f,
        "non-finite throttle becomes zero");
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

void TestVehicleControlTargetSelection() {
  rtc_console::VehicleControlTargetRegistry targets;
  targets.SetP2PConnected(7, true);
  targets.SetControlChannelOpen(7, true);
  targets.SetP2PConnected(8, true);
  targets.SetControlChannelOpen(8, true);

  const std::vector<uint32_t> available = targets.AvailableTargets();
  Check(available.size() == 2 && available[0] == 7 && available[1] == 8,
        "list all ready vehicle targets");
  Check(targets.selected_target() == 0,
        "callbacks do not select a vehicle implicitly");
  Check(targets.SelectTarget(7), "select an explicitly chosen target");
  Check(targets.selected_target_ready(), "selected target is ready");

  targets.SetControlChannelOpen(8, false);
  Check(targets.selected_target() == 7,
        "unrelated channel state does not replace selection");
  Check(!targets.SetP2PConnected(8, false),
        "unrelated disconnect does not remove selection");
  Check(targets.selected_target() == 7 &&
            targets.selected_target_ready(),
        "selected target remains ready after unrelated disconnect");
  Check(targets.SetP2PConnected(7, false),
        "selected disconnect reports vehicle control reset");
  Check(targets.selected_target() == 0,
        "disconnect clears the selected target");
  Check(!targets.SelectTarget(8), "closed channel cannot be selected");
}

}  // namespace

int main() {
  TestForwardLeft();
  TestConflictingInputsStop();
  TestThrottleClamp();
  TestEmergencyStop();
  TestVehicleControlTargetSelection();
  std::cout << "rtc_console_vehicle_control_sender_tests passed"
            << std::endl;
  return 0;
}
