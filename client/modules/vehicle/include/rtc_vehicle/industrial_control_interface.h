#pragma once

#include "rtc_control/vehicle_control_protocol.h"

#include <functional>
#include <string>

namespace rtc_vehicle {

struct IndustrialCommandResult {
  bool accepted = false;
  vts_rtc::control::VehicleErrorCode error_code =
      vts_rtc::control::VehicleErrorCode::InvalidState;
  std::string detail;
};

class IndustrialControlInterface {
 public:
  virtual ~IndustrialControlInterface() = default;

  virtual bool Open(std::string* error_message) = 0;
  virtual void Close() = 0;
  virtual IndustrialCommandResult SendDriveCommand(
      const vts_rtc::control::DriveCommand& command) = 0;
  virtual IndustrialCommandResult SendGearCommand(
      vts_rtc::control::VehicleGear gear) = 0;
  virtual void SendStop() = 0;
};

// 工控机通信尚未接入时使用的占位实现。它允许主程序运行摄像头和 RTC，
// 但会拒绝所有运动及换档指令，避免误报设备已经执行。
class PlaceholderIndustrialControlInterface final
    : public IndustrialControlInterface {
 public:
  using LogCallback = std::function<void(const std::string&)>;

  explicit PlaceholderIndustrialControlInterface(const LogCallback& logger);

  bool Open(std::string* error_message) override;
  void Close() override;
  IndustrialCommandResult SendDriveCommand(
      const vts_rtc::control::DriveCommand& command) override;
  IndustrialCommandResult SendGearCommand(
      vts_rtc::control::VehicleGear gear) override;
  void SendStop() override;

 private:
  void LogOnce();

  LogCallback logger_;
  bool opened_ = false;
  bool unavailable_logged_ = false;
};

}  // 命名空间 rtc_vehicle
