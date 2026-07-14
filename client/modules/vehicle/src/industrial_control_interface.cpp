#include "rtc_vehicle/industrial_control_interface.h"

namespace rtc_vehicle {

namespace {

IndustrialCommandResult NotConnectedResult() {
  IndustrialCommandResult result;
  result.accepted = false;
  result.error_code = vts_rtc::control::VehicleErrorCode::InvalidState;
  result.detail = "工控机控制接口尚未接入";
  return result;
}

}  // 匿名命名空间

PlaceholderIndustrialControlInterface::PlaceholderIndustrialControlInterface(
    const LogCallback& logger)
    : logger_(logger) {}

bool PlaceholderIndustrialControlInterface::Open(std::string* error_message) {
  opened_ = true;
  unavailable_logged_ = false;
  if (error_message) {
    error_message->clear();
  }
  if (logger_) {
    logger_("工控机接口运行在占位模式，不会下发实际控制指令");
  }
  return true;
}

void PlaceholderIndustrialControlInterface::Close() {
  opened_ = false;
}

IndustrialCommandResult
PlaceholderIndustrialControlInterface::SendDriveCommand(
    const vts_rtc::control::DriveCommand&) {
  LogOnce();
  return NotConnectedResult();
}

IndustrialCommandResult
PlaceholderIndustrialControlInterface::SendGearCommand(
    vts_rtc::control::VehicleGear) {
  LogOnce();
  return NotConnectedResult();
}

void PlaceholderIndustrialControlInterface::SendStop() {
  if (opened_ && logger_) {
    logger_("工控机占位接口收到停车请求");
  }
}

void PlaceholderIndustrialControlInterface::LogOnce() {
  if (!unavailable_logged_ && logger_) {
    logger_("控制指令未下发：工控机接口尚未实现");
    unavailable_logged_ = true;
  }
}

}  // 命名空间 rtc_vehicle
