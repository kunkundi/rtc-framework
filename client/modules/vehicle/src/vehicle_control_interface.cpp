#include "rtc_vehicle/vehicle_control_interface.h"

namespace rtc_vehicle {

namespace {

VehicleCommandResult NotConnectedResult() {
  VehicleCommandResult result;
  result.accepted = false;
  result.error_code = vts_rtc::vehicle::VehicleErrorCode::InvalidState;
  result.detail = "vehicle control interface is not implemented";
  return result;
}

}  // 匿名命名空间

PlaceholderVehicleControlInterface::PlaceholderVehicleControlInterface(
    const LogCallback& logger)
    : logger_(logger) {}

bool PlaceholderVehicleControlInterface::Open(std::string* error_message) {
  opened_ = true;
  unavailable_logged_ = false;
  if (error_message) {
    error_message->clear();
  }
  if (logger_) {
    logger_("Vehicle control interface is running in placeholder mode; "
            "commands will not be sent to hardware");
  }
  return true;
}

void PlaceholderVehicleControlInterface::Close() {
  opened_ = false;
}

VehicleCommandResult
PlaceholderVehicleControlInterface::SendDriveCommand(
    const vts_rtc::vehicle::DriveCommand&) {
  LogOnce();
  return NotConnectedResult();
}

VehicleCommandResult
PlaceholderVehicleControlInterface::SendGearCommand(
    vts_rtc::vehicle::VehicleGear) {
  LogOnce();
  return NotConnectedResult();
}

VehicleCommandResult
PlaceholderVehicleControlInterface::SendDogAction(
    const vts_rtc::vehicle::DogAction&) {
  LogOnce();
  return NotConnectedResult();
}

void PlaceholderVehicleControlInterface::SendStop() {
  if (opened_ && logger_) {
    logger_("Placeholder vehicle control interface received a stop request");
  }
}

void PlaceholderVehicleControlInterface::LogOnce() {
  if (!unavailable_logged_ && logger_) {
    logger_("Control command was not sent because the vehicle control "
            "interface is not implemented");
    unavailable_logged_ = true;
  }
}

}  // 命名空间 rtc_vehicle
