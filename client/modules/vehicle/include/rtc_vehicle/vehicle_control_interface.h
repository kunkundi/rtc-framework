#pragma once

#include "rtc_vehicle_protocol/vehicle_control_protocol.h"

#include <functional>
#include <string>

namespace rtc_vehicle {

struct VehicleCommandResult {
  bool accepted = false;
  vts_rtc::vehicle::VehicleErrorCode error_code =
      vts_rtc::vehicle::VehicleErrorCode::InvalidState;
  std::string detail;
};

class VehicleControlInterface {
 public:
  virtual ~VehicleControlInterface() = default;

  virtual bool Open(std::string* error_message) = 0;
  virtual void Close() = 0;
  virtual VehicleCommandResult SendDriveCommand(
      const vts_rtc::vehicle::DriveCommand& command) = 0;
  virtual VehicleCommandResult SendGearCommand(
      vts_rtc::vehicle::VehicleGear gear) = 0;
  virtual void SendStop() = 0;
};

// 车辆控制链路尚未接入时使用的占位实现。它允许主程序运行摄像头和 RTC，
// 但会拒绝所有运动及换档指令，避免误报设备已经执行。
class PlaceholderVehicleControlInterface final
    : public VehicleControlInterface {
 public:
  using LogCallback = std::function<void(const std::string&)>;

  explicit PlaceholderVehicleControlInterface(const LogCallback& logger);

  bool Open(std::string* error_message) override;
  void Close() override;
  VehicleCommandResult SendDriveCommand(
      const vts_rtc::vehicle::DriveCommand& command) override;
  VehicleCommandResult SendGearCommand(
      vts_rtc::vehicle::VehicleGear gear) override;
  void SendStop() override;

 private:
  void LogOnce();

  LogCallback logger_;
  bool opened_ = false;
  bool unavailable_logged_ = false;
};

}  // 命名空间 rtc_vehicle
