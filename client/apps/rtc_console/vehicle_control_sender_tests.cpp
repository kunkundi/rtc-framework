#include "vehicle_control_sender.h"

#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

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

struct SentDogAction {
  vts_rtc::vehicle::DogActionType action =
      vts_rtc::vehicle::DogActionType::Unknown;
  bool log_success = false;
};

void TestGamepadDogActionRetriesFailedTransitions() {
  rtc_console::GamepadDogActionState state(100);
  std::vector<SentDogAction> sent;
  bool accept = false;
  const auto send = [&sent, &accept](
                        vts_rtc::vehicle::DogActionType action,
                        float,
                        bool log_success) {
    sent.push_back({action, log_success});
    return accept;
  };

  state.Update(true, false, false, false, 0.5f, 1000, send);
  Check(!state.lateral_active(), "failed lateral start keeps retry state");
  state.Update(true, false, false, false, 0.5f, 1010, send);
  Check(sent.size() == 2, "failed lateral start retries on next update");

  accept = true;
  state.Update(true, false, false, false, 0.5f, 1020, send);
  Check(state.lateral_active(), "successful lateral start commits state");
  state.Update(true, false, false, false, 0.5f, 1119, send);
  Check(sent.size() == 3, "lateral heartbeat waits for interval");
  state.Update(true, false, false, false, 0.5f, 1120, send);
  Check(sent.size() == 4 && !sent.back().log_success,
        "lateral heartbeat is sent without duplicate success log");

  accept = false;
  state.Update(false, false, false, false, 0.5f, 1130, send);
  Check(state.lateral_active(), "failed lateral stop preserves active state");
  state.Update(false, false, false, false, 0.5f, 1140, send);
  Check(sent.size() == 6, "failed lateral stop retries on next update");

  accept = true;
  state.Update(false, false, false, false, 0.5f, 1150, send);
  Check(!state.lateral_active(), "successful lateral stop clears state");
}

void TestGamepadDisconnectRetriesLateralStop() {
  rtc_console::GamepadDogActionState state(100);
  bool accept = true;
  int stop_attempts = 0;
  const auto send = [&accept, &stop_attempts](
                        vts_rtc::vehicle::DogActionType action,
                        float,
                        bool) {
    if (action == vts_rtc::vehicle::DogActionType::LateralStop) {
      ++stop_attempts;
    }
    return accept;
  };

  state.Update(false, true, false, false, 0.5f, 2000, send);
  Check(state.lateral_active(), "prepare active lateral state");

  accept = false;
  Check(!state.StopForDisconnect(send),
        "failed disconnect stop reports pending state");
  Check(state.lateral_active(),
        "failed disconnect stop keeps lateral state for retry");

  accept = true;
  Check(state.StopForDisconnect(send),
        "disconnect stop succeeds after transport recovers");
  Check(!state.lateral_active() && stop_attempts == 2,
        "disconnect stop retries until successful");
}

}  // namespace

int main() {
  TestForwardLeft();
  TestConflictingInputsStop();
  TestThrottleClamp();
  TestEmergencyStop();
  TestVehicleControlTargetSelection();
  TestGamepadDogActionRetriesFailedTransitions();
  TestGamepadDisconnectRetriesLateralStop();
  std::cout << "rtc_console_vehicle_control_sender_tests passed"
            << std::endl;
  return 0;
}
