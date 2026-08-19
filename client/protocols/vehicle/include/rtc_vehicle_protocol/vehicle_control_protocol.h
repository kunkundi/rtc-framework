#pragma once

#include <stdint.h>

#include <string>
#include <vector>

namespace vts_rtc {
namespace vehicle {

constexpr const char* kVehicleControlChannelLabel = "vehicle.control.v1";
constexpr const char* kVehicleEventChannelLabel = "vehicle.event.v1";
constexpr const char* kVehicleStateChannelLabel = "vehicle.state.v1";

constexpr uint32_t kVehicleControlMagic = 0x314c4356u;
constexpr uint32_t kVehicleControlProtocolMajor = 1;
constexpr uint32_t kVehicleControlProtocolMinor = 0;
constexpr uint32_t kDefaultDriveWatchdogMs = 300;

enum class MessageType : uint32_t {
  Unknown = 0,
  DriveCommand = 1,
  SetGear = 2,
  EventAck = 3,
  VehicleState = 4,
  DogAction = 5,
};

enum class DriveDirection : uint32_t {
  Stop = 0,
  Forward = 1,
  Reverse = 2,
};

enum class SteeringDirection : uint32_t {
  Center = 0,
  Left = 1,
  Right = 2,
};

enum class VehicleGear : uint32_t {
  Neutral = 0,
  Forward = 1,
  Reverse = 2,
};

enum class VehicleErrorCode : uint32_t {
  None = 0,
  InvalidArgument = 1,
  InvalidState = 2,
  Internal = 3,
};

enum class DogActionType : uint32_t {
  Unknown = 0,
  LateralLeft = 1,
  LateralRight = 2,
  LateralStop = 3,
  Stand = 4,
  LieDown = 5,
};

enum class DecodeStatus {
  Ok,
  EmptyPayload,
  DecodeFailed,
  InvalidEnvelope,
  UnsupportedProtocol,
  UnexpectedPayload,
  InvalidField,
};

struct DriveCommand {
  DriveDirection drive_direction = DriveDirection::Stop;
  SteeringDirection steering_direction = SteeringDirection::Center;
  float throttle = 0.0f;
  float brake = 0.0f;
};

struct SetGear {
  uint64_t request_id = 0;
  VehicleGear gear = VehicleGear::Neutral;
};

struct DogAction {
  uint64_t request_id = 0;
  DogActionType action = DogActionType::Unknown;
  float speed = 0.0f;
};

struct EventAck {
  uint64_t request_id = 0;
  bool accepted = false;
  VehicleErrorCode error_code = VehicleErrorCode::None;
  VehicleGear active_gear = VehicleGear::Neutral;
};

struct VehicleState {
  VehicleGear active_gear = VehicleGear::Neutral;
  uint64_t last_received_drive_seq = 0;
  bool watchdog_stopped = false;
};

struct Envelope {
  uint64_t seq = 0;
  MessageType type = MessageType::Unknown;
  DriveCommand drive_command;
  SetGear set_gear;
  DogAction dog_action;
  EventAck event_ack;
  VehicleState vehicle_state;
};

struct EncodeResult {
  std::vector<uint8_t> payload;
  std::string error_message;

  explicit operator bool() const { return error_message.empty(); }
};

struct DecodeResult {
  DecodeStatus status = DecodeStatus::DecodeFailed;
  Envelope envelope;
  std::string error_message;

  explicit operator bool() const { return status == DecodeStatus::Ok; }
};

struct ValidationResult {
  bool valid = false;
  std::string error_message;

  explicit operator bool() const { return valid; }
};

EncodeResult EncodeDriveCommand(uint64_t seq, const DriveCommand& command);
EncodeResult EncodeSetGear(uint64_t seq, const SetGear& set_gear);
EncodeResult EncodeDogAction(uint64_t seq, const DogAction& dog_action);
EncodeResult EncodeEventAck(uint64_t seq, const EventAck& event_ack);
EncodeResult EncodeVehicleState(uint64_t seq, const VehicleState& state);
DecodeResult DecodeEnvelope(const uint8_t* data, size_t size);

inline DecodeResult DecodeEnvelope(const std::vector<uint8_t>& payload) {
  return DecodeEnvelope(payload.data(), payload.size());
}

ValidationResult ValidateDriveCommand(const DriveCommand& command);
ValidationResult ValidateSetGear(const SetGear& set_gear);
ValidationResult ValidateDogAction(const DogAction& dog_action);

enum class DriveReceiveStatus {
  Accepted,
  NotStarted,
  DuplicateOrOutOfOrder,
  InvalidCommand,
  WatchdogExpired,
};

struct DriveReceiveResult {
  DriveReceiveStatus status = DriveReceiveStatus::NotStarted;
  bool should_stop = true;
  std::string error_message;
};

// 仅跟踪协议层的存活状态和序号。实际制动、电机禁用和物理安全联锁由设备控制器负责。
class DriveCommandGate {
 public:
  explicit DriveCommandGate(uint32_t watchdog_ms = kDefaultDriveWatchdogMs);

  void Start(uint64_t now_ms);
  void Stop();
  DriveReceiveResult Accept(uint64_t seq,
                            const DriveCommand& command,
                            uint64_t now_ms);
  // 看门狗停车后仅用更大的合法序号恢复，不重置历史序号。
  DriveReceiveResult Recover(uint64_t seq,
                             const DriveCommand& command,
                             uint64_t now_ms);
  bool PollWatchdog(uint64_t now_ms);

  bool started() const { return started_; }
  bool watchdog_stopped() const { return watchdog_stopped_; }
  uint64_t last_received_seq() const { return last_received_seq_; }

 private:
  uint32_t watchdog_ms_;
  bool started_ = false;
  bool watchdog_stopped_ = true;
  bool has_received_drive_ = false;
  uint64_t last_received_seq_ = 0;
  uint64_t last_valid_command_ms_ = 0;
};

}  // 命名空间 vehicle
}  // 命名空间 vts_rtc
