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

DebugVehicleControlInterface::DebugVehicleControlInterface(const LogCallback& logger)
    : logger_(logger) {}

bool DebugVehicleControlInterface::Open(std::string* error_message) {
  opened_ = true;
  if (error_message) error_message->clear();
  if (logger_) logger_("DEBUG vehicle simulation enabled; no hardware commands are sent");
  return true;
}

void DebugVehicleControlInterface::Close() { opened_ = false; }

VehicleCommandResult DebugVehicleControlInterface::SendDriveCommand(
    const vts_rtc::vehicle::DriveCommand& command) {
  const auto valid = vts_rtc::vehicle::ValidateDriveCommand(command);
  VehicleCommandResult result;
  result.accepted = opened_ && static_cast<bool>(valid);
  result.error_code = result.accepted ? vts_rtc::vehicle::VehicleErrorCode::None
      : !opened_ ? vts_rtc::vehicle::VehicleErrorCode::InvalidState
                 : vts_rtc::vehicle::VehicleErrorCode::InvalidArgument;
  result.detail = "DEBUG simulation only";
  return result;
}

VehicleCommandResult DebugVehicleControlInterface::SendGearCommand(
    vts_rtc::vehicle::VehicleGear gear) {
  vts_rtc::vehicle::SetGear request;
  request.request_id = 1;
  request.gear = gear;
  const auto valid = vts_rtc::vehicle::ValidateSetGear(request);
  VehicleCommandResult result;
  result.accepted = opened_ && static_cast<bool>(valid);
  result.error_code = result.accepted ? vts_rtc::vehicle::VehicleErrorCode::None
      : !opened_ ? vts_rtc::vehicle::VehicleErrorCode::InvalidState
                 : vts_rtc::vehicle::VehicleErrorCode::InvalidArgument;
  result.detail = "DEBUG simulation only";
  if (result.accepted && logger_) logger_("DEBUG simulated gear change accepted");
  return result;
}

VehicleCommandResult DebugVehicleControlInterface::SendDogAction(
    const vts_rtc::vehicle::DogAction&) {
  return NotConnectedResult();
}

void DebugVehicleControlInterface::SendStop() {
  if (opened_ && logger_) logger_("DEBUG simulated stop requested");
}

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
