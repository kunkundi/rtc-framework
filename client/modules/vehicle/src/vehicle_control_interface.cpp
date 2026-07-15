#include "rtc_vehicle/vehicle_control_interface.h"

namespace rtc_vehicle {

namespace {

VehicleCommandResult NotConnectedResult() {
  VehicleCommandResult result;
  result.accepted = false;
  result.error_code = vts_rtc::vehicle::VehicleErrorCode::InvalidState;
  result.detail = "车辆控制接口尚未接入";
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
    logger_("车辆控制接口运行在占位模式，不会下发实际控制指令");
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

void PlaceholderVehicleControlInterface::SendStop() {
  if (opened_ && logger_) {
    logger_("车辆控制占位接口收到停车请求");
  }
}

void PlaceholderVehicleControlInterface::LogOnce() {
  if (!unavailable_logged_ && logger_) {
    logger_("控制指令未下发：车辆控制接口尚未实现");
    unavailable_logged_ = true;
  }
}

}  // 命名空间 rtc_vehicle
