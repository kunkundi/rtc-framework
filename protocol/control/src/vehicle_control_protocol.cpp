#include "rtc_control/vehicle_control_protocol.h"

#include "rtc_control.pb.h"

#include <pb_decode.h>
#include <pb_encode.h>

#include <cmath>
#include <sstream>

namespace vts_rtc {
namespace control {
namespace {

using PbDriveCommand = vtsrtc_control_v1_DriveCommand;
using PbDriveDirection = vtsrtc_control_v1_DriveDirection;
using PbEnvelope = vtsrtc_control_v1_VehicleControlEnvelope;
using PbErrorCode = vtsrtc_control_v1_VehicleErrorCode;
using PbEventAck = vtsrtc_control_v1_EventAck;
using PbGear = vtsrtc_control_v1_VehicleGear;
using PbMessageType = vtsrtc_control_v1_VehicleMessageType;
using PbSetGear = vtsrtc_control_v1_SetGear;
using PbState = vtsrtc_control_v1_VehicleState;
using PbSteeringDirection = vtsrtc_control_v1_SteeringDirection;

template <typename T>
void Reset(T* target) {
  *target = T{};
}

std::string Error(const char* field, const char* reason) {
  std::ostringstream stream;
  stream << field << ": " << reason;
  return stream.str();
}

bool IsFinite(float value) {
  return std::isfinite(value) != 0;
}

PbMessageType ToPbMessageType(MessageType type) {
  switch (type) {
    case MessageType::DriveCommand:
      return vtsrtc_control_v1_VehicleMessageType_VEHICLE_MESSAGE_TYPE_DRIVE_COMMAND;
    case MessageType::SetGear:
      return vtsrtc_control_v1_VehicleMessageType_VEHICLE_MESSAGE_TYPE_SET_GEAR;
    case MessageType::EventAck:
      return vtsrtc_control_v1_VehicleMessageType_VEHICLE_MESSAGE_TYPE_EVENT_ACK;
    case MessageType::VehicleState:
      return vtsrtc_control_v1_VehicleMessageType_VEHICLE_MESSAGE_TYPE_VEHICLE_STATE;
    case MessageType::Unknown:
    default:
      return vtsrtc_control_v1_VehicleMessageType_VEHICLE_MESSAGE_TYPE_UNKNOWN;
  }
}

MessageType FromPbMessageType(PbMessageType type) {
  switch (type) {
    case vtsrtc_control_v1_VehicleMessageType_VEHICLE_MESSAGE_TYPE_DRIVE_COMMAND:
      return MessageType::DriveCommand;
    case vtsrtc_control_v1_VehicleMessageType_VEHICLE_MESSAGE_TYPE_SET_GEAR:
      return MessageType::SetGear;
    case vtsrtc_control_v1_VehicleMessageType_VEHICLE_MESSAGE_TYPE_EVENT_ACK:
      return MessageType::EventAck;
    case vtsrtc_control_v1_VehicleMessageType_VEHICLE_MESSAGE_TYPE_VEHICLE_STATE:
      return MessageType::VehicleState;
    case vtsrtc_control_v1_VehicleMessageType_VEHICLE_MESSAGE_TYPE_UNKNOWN:
    default:
      return MessageType::Unknown;
  }
}

PbDriveDirection ToPbDriveDirection(DriveDirection direction) {
  switch (direction) {
    case DriveDirection::Forward:
      return vtsrtc_control_v1_DriveDirection_DRIVE_DIRECTION_FORWARD;
    case DriveDirection::Reverse:
      return vtsrtc_control_v1_DriveDirection_DRIVE_DIRECTION_REVERSE;
    case DriveDirection::Stop:
    default:
      return vtsrtc_control_v1_DriveDirection_DRIVE_DIRECTION_STOP;
  }
}

DriveDirection FromPbDriveDirection(PbDriveDirection direction) {
  switch (direction) {
    case vtsrtc_control_v1_DriveDirection_DRIVE_DIRECTION_FORWARD:
      return DriveDirection::Forward;
    case vtsrtc_control_v1_DriveDirection_DRIVE_DIRECTION_REVERSE:
      return DriveDirection::Reverse;
    case vtsrtc_control_v1_DriveDirection_DRIVE_DIRECTION_STOP:
    default:
      return DriveDirection::Stop;
  }
}

PbSteeringDirection ToPbSteeringDirection(SteeringDirection direction) {
  switch (direction) {
    case SteeringDirection::Left:
      return vtsrtc_control_v1_SteeringDirection_STEERING_DIRECTION_LEFT;
    case SteeringDirection::Right:
      return vtsrtc_control_v1_SteeringDirection_STEERING_DIRECTION_RIGHT;
    case SteeringDirection::Center:
    default:
      return vtsrtc_control_v1_SteeringDirection_STEERING_DIRECTION_CENTER;
  }
}

SteeringDirection FromPbSteeringDirection(PbSteeringDirection direction) {
  switch (direction) {
    case vtsrtc_control_v1_SteeringDirection_STEERING_DIRECTION_LEFT:
      return SteeringDirection::Left;
    case vtsrtc_control_v1_SteeringDirection_STEERING_DIRECTION_RIGHT:
      return SteeringDirection::Right;
    case vtsrtc_control_v1_SteeringDirection_STEERING_DIRECTION_CENTER:
    default:
      return SteeringDirection::Center;
  }
}

PbGear ToPbGear(VehicleGear gear) {
  switch (gear) {
    case VehicleGear::Forward:
      return vtsrtc_control_v1_VehicleGear_VEHICLE_GEAR_FORWARD;
    case VehicleGear::Reverse:
      return vtsrtc_control_v1_VehicleGear_VEHICLE_GEAR_REVERSE;
    case VehicleGear::Neutral:
    default:
      return vtsrtc_control_v1_VehicleGear_VEHICLE_GEAR_NEUTRAL;
  }
}

VehicleGear FromPbGear(PbGear gear) {
  switch (gear) {
    case vtsrtc_control_v1_VehicleGear_VEHICLE_GEAR_FORWARD:
      return VehicleGear::Forward;
    case vtsrtc_control_v1_VehicleGear_VEHICLE_GEAR_REVERSE:
      return VehicleGear::Reverse;
    case vtsrtc_control_v1_VehicleGear_VEHICLE_GEAR_NEUTRAL:
    default:
      return VehicleGear::Neutral;
  }
}

PbErrorCode ToPbErrorCode(VehicleErrorCode code) {
  switch (code) {
    case VehicleErrorCode::InvalidArgument:
      return vtsrtc_control_v1_VehicleErrorCode_VEHICLE_ERROR_CODE_INVALID_ARGUMENT;
    case VehicleErrorCode::InvalidState:
      return vtsrtc_control_v1_VehicleErrorCode_VEHICLE_ERROR_CODE_INVALID_STATE;
    case VehicleErrorCode::Internal:
      return vtsrtc_control_v1_VehicleErrorCode_VEHICLE_ERROR_CODE_INTERNAL;
    case VehicleErrorCode::None:
    default:
      return vtsrtc_control_v1_VehicleErrorCode_VEHICLE_ERROR_CODE_NONE;
  }
}

VehicleErrorCode FromPbErrorCode(PbErrorCode code) {
  switch (code) {
    case vtsrtc_control_v1_VehicleErrorCode_VEHICLE_ERROR_CODE_INVALID_ARGUMENT:
      return VehicleErrorCode::InvalidArgument;
    case vtsrtc_control_v1_VehicleErrorCode_VEHICLE_ERROR_CODE_INVALID_STATE:
      return VehicleErrorCode::InvalidState;
    case vtsrtc_control_v1_VehicleErrorCode_VEHICLE_ERROR_CODE_INTERNAL:
      return VehicleErrorCode::Internal;
    case vtsrtc_control_v1_VehicleErrorCode_VEHICLE_ERROR_CODE_NONE:
    default:
      return VehicleErrorCode::None;
  }
}

bool IsKnownMessageType(MessageType type) {
  return type == MessageType::DriveCommand || type == MessageType::SetGear ||
         type == MessageType::EventAck || type == MessageType::VehicleState;
}

bool IsKnownDriveDirection(DriveDirection direction) {
  return direction == DriveDirection::Stop || direction == DriveDirection::Forward ||
         direction == DriveDirection::Reverse;
}

bool IsKnownDriveDirection(PbDriveDirection direction) {
  return direction == vtsrtc_control_v1_DriveDirection_DRIVE_DIRECTION_STOP ||
         direction == vtsrtc_control_v1_DriveDirection_DRIVE_DIRECTION_FORWARD ||
         direction == vtsrtc_control_v1_DriveDirection_DRIVE_DIRECTION_REVERSE;
}

bool IsKnownSteeringDirection(SteeringDirection direction) {
  return direction == SteeringDirection::Center || direction == SteeringDirection::Left ||
         direction == SteeringDirection::Right;
}

bool IsKnownSteeringDirection(PbSteeringDirection direction) {
  return direction == vtsrtc_control_v1_SteeringDirection_STEERING_DIRECTION_CENTER ||
         direction == vtsrtc_control_v1_SteeringDirection_STEERING_DIRECTION_LEFT ||
         direction == vtsrtc_control_v1_SteeringDirection_STEERING_DIRECTION_RIGHT;
}

bool IsKnownGear(VehicleGear gear) {
  return gear == VehicleGear::Neutral || gear == VehicleGear::Forward ||
         gear == VehicleGear::Reverse;
}

bool IsKnownGear(PbGear gear) {
  return gear == vtsrtc_control_v1_VehicleGear_VEHICLE_GEAR_NEUTRAL ||
         gear == vtsrtc_control_v1_VehicleGear_VEHICLE_GEAR_FORWARD ||
         gear == vtsrtc_control_v1_VehicleGear_VEHICLE_GEAR_REVERSE;
}

bool IsKnownErrorCode(VehicleErrorCode code) {
  return code == VehicleErrorCode::None ||
         code == VehicleErrorCode::InvalidArgument ||
         code == VehicleErrorCode::InvalidState ||
         code == VehicleErrorCode::Internal;
}

bool IsKnownErrorCode(PbErrorCode code) {
  return code == vtsrtc_control_v1_VehicleErrorCode_VEHICLE_ERROR_CODE_NONE ||
         code == vtsrtc_control_v1_VehicleErrorCode_VEHICLE_ERROR_CODE_INVALID_ARGUMENT ||
         code == vtsrtc_control_v1_VehicleErrorCode_VEHICLE_ERROR_CODE_INVALID_STATE ||
         code == vtsrtc_control_v1_VehicleErrorCode_VEHICLE_ERROR_CODE_INTERNAL;
}

void FillHeader(PbEnvelope* envelope, MessageType type, uint64_t seq) {
  envelope->has_magic = true;
  envelope->magic = kVehicleControlMagic;
  envelope->has_protocol_major = true;
  envelope->protocol_major = kVehicleControlProtocolMajor;
  envelope->has_protocol_minor = true;
  envelope->protocol_minor = kVehicleControlProtocolMinor;
  envelope->has_type = true;
  envelope->type = ToPbMessageType(type);
  envelope->has_seq = true;
  envelope->seq = seq;
}

EncodeResult EncodePbEnvelope(const PbEnvelope& envelope) {
  EncodeResult result;
  result.payload.resize(vtsrtc_control_v1_VehicleControlEnvelope_size);
  pb_ostream_t stream =
      pb_ostream_from_buffer(result.payload.data(), result.payload.size());
  if (!pb_encode(&stream, vtsrtc_control_v1_VehicleControlEnvelope_fields,
                 &envelope)) {
    result.payload.clear();
    result.error_message = PB_GET_ERROR(&stream);
    return result;
  }
  result.payload.resize(stream.bytes_written);
  return result;
}

DecodeResult DecodeError(DecodeStatus status, const std::string& error) {
  DecodeResult result;
  result.status = status;
  result.error_message = error;
  return result;
}

bool FillDriveCommand(const DriveCommand& source,
                      PbDriveCommand* target,
                      std::string* error) {
  const ValidationResult validation = ValidateDriveCommand(source);
  if (!validation) {
    *error = validation.error_message;
    return false;
  }
  Reset(target);
  target->has_drive_direction = true;
  target->drive_direction = ToPbDriveDirection(source.drive_direction);
  target->has_steering_direction = true;
  target->steering_direction = ToPbSteeringDirection(source.steering_direction);
  target->has_throttle = true;
  target->throttle = source.throttle;
  target->has_brake = true;
  target->brake = source.brake;
  return true;
}

bool FillSetGear(const SetGear& source, PbSetGear* target, std::string* error) {
  const ValidationResult validation = ValidateSetGear(source);
  if (!validation) {
    *error = validation.error_message;
    return false;
  }
  Reset(target);
  target->has_request_id = true;
  target->request_id = source.request_id;
  target->has_gear = true;
  target->gear = ToPbGear(source.gear);
  return true;
}

bool FillEventAck(const EventAck& source, PbEventAck* target,
                  std::string* error) {
  if (source.request_id == 0) {
    *error = Error("event_ack.request_id", "must be non-zero");
    return false;
  }
  if (!IsKnownErrorCode(source.error_code) || !IsKnownGear(source.active_gear)) {
    *error = Error("event_ack", "contains an unknown enum value");
    return false;
  }
  if ((source.accepted && source.error_code != VehicleErrorCode::None) ||
      (!source.accepted && source.error_code == VehicleErrorCode::None)) {
    *error = Error("event_ack", "acceptance and error_code are inconsistent");
    return false;
  }
  Reset(target);
  target->has_request_id = true;
  target->request_id = source.request_id;
  target->has_accepted = true;
  target->accepted = source.accepted;
  target->has_error_code = true;
  target->error_code = ToPbErrorCode(source.error_code);
  target->has_active_gear = true;
  target->active_gear = ToPbGear(source.active_gear);
  return true;
}

bool FillVehicleState(const VehicleState& source, PbState* target,
                      std::string* error) {
  if (!IsKnownGear(source.active_gear)) {
    *error = Error("vehicle_state.active_gear", "is unknown");
    return false;
  }
  Reset(target);
  target->has_active_gear = true;
  target->active_gear = ToPbGear(source.active_gear);
  target->has_last_received_drive_seq = true;
  target->last_received_drive_seq = source.last_received_drive_seq;
  target->has_watchdog_stopped = true;
  target->watchdog_stopped = source.watchdog_stopped;
  return true;
}

}  // 匿名命名空间

ValidationResult ValidateDriveCommand(const DriveCommand& command) {
  if (!IsKnownDriveDirection(command.drive_direction)) {
    return {false, Error("drive_command.drive_direction", "is unknown")};
  }
  if (!IsKnownSteeringDirection(command.steering_direction)) {
    return {false, Error("drive_command.steering_direction", "is unknown")};
  }
  if (!IsFinite(command.throttle) || command.throttle < 0.0f ||
      command.throttle > 1.0f) {
    return {false, Error("drive_command.throttle", "must be finite and in range 0..1")};
  }
  if (!IsFinite(command.brake) || command.brake < 0.0f ||
      command.brake > 1.0f) {
    return {false, Error("drive_command.brake", "must be finite and in range 0..1")};
  }
  return {true, ""};
}

ValidationResult ValidateSetGear(const SetGear& set_gear) {
  if (set_gear.request_id == 0) {
    return {false, Error("set_gear.request_id", "must be non-zero")};
  }
  if (!IsKnownGear(set_gear.gear)) {
    return {false, Error("set_gear.gear", "is unknown")};
  }
  return {true, ""};
}

EncodeResult EncodeDriveCommand(uint64_t seq, const DriveCommand& command) {
  PbEnvelope envelope = vtsrtc_control_v1_VehicleControlEnvelope_init_zero;
  FillHeader(&envelope, MessageType::DriveCommand, seq);
  envelope.which_payload = vtsrtc_control_v1_VehicleControlEnvelope_drive_command_tag;
  std::string error;
  if (!FillDriveCommand(command, &envelope.payload.drive_command, &error)) {
    return {{}, error};
  }
  return EncodePbEnvelope(envelope);
}

EncodeResult EncodeSetGear(uint64_t seq, const SetGear& set_gear) {
  PbEnvelope envelope = vtsrtc_control_v1_VehicleControlEnvelope_init_zero;
  FillHeader(&envelope, MessageType::SetGear, seq);
  envelope.which_payload = vtsrtc_control_v1_VehicleControlEnvelope_set_gear_tag;
  std::string error;
  if (!FillSetGear(set_gear, &envelope.payload.set_gear, &error)) {
    return {{}, error};
  }
  return EncodePbEnvelope(envelope);
}

EncodeResult EncodeEventAck(uint64_t seq, const EventAck& event_ack) {
  PbEnvelope envelope = vtsrtc_control_v1_VehicleControlEnvelope_init_zero;
  FillHeader(&envelope, MessageType::EventAck, seq);
  envelope.which_payload = vtsrtc_control_v1_VehicleControlEnvelope_event_ack_tag;
  std::string error;
  if (!FillEventAck(event_ack, &envelope.payload.event_ack, &error)) {
    return {{}, error};
  }
  return EncodePbEnvelope(envelope);
}

EncodeResult EncodeVehicleState(uint64_t seq, const VehicleState& state) {
  PbEnvelope envelope = vtsrtc_control_v1_VehicleControlEnvelope_init_zero;
  FillHeader(&envelope, MessageType::VehicleState, seq);
  envelope.which_payload = vtsrtc_control_v1_VehicleControlEnvelope_vehicle_state_tag;
  std::string error;
  if (!FillVehicleState(state, &envelope.payload.vehicle_state, &error)) {
    return {{}, error};
  }
  return EncodePbEnvelope(envelope);
}

DecodeResult DecodeEnvelope(const uint8_t* data, size_t size) {
  if (size == 0 || !data) {
    return DecodeError(DecodeStatus::EmptyPayload, "empty payload");
  }

  PbEnvelope envelope = vtsrtc_control_v1_VehicleControlEnvelope_init_zero;
  pb_istream_t stream = pb_istream_from_buffer(data, size);
  if (!pb_decode(&stream, vtsrtc_control_v1_VehicleControlEnvelope_fields,
                 &envelope)) {
    return DecodeError(DecodeStatus::DecodeFailed, PB_GET_ERROR(&stream));
  }
  if (!envelope.has_magic || envelope.magic != kVehicleControlMagic) {
    return DecodeError(DecodeStatus::InvalidEnvelope, "missing or invalid magic");
  }
  if (!envelope.has_protocol_major) {
    return DecodeError(DecodeStatus::InvalidEnvelope, "missing protocol_major");
  }
  if (envelope.protocol_major != kVehicleControlProtocolMajor) {
    return DecodeError(DecodeStatus::UnsupportedProtocol,
                       "unsupported protocol_major");
  }
  if (!envelope.has_type) {
    return DecodeError(DecodeStatus::InvalidEnvelope, "missing type");
  }

  const MessageType type = FromPbMessageType(envelope.type);
  if (!IsKnownMessageType(type)) {
    return DecodeError(DecodeStatus::InvalidEnvelope, "unknown message type");
  }

  DecodeResult result;
  result.status = DecodeStatus::Ok;
  result.envelope.seq = envelope.has_seq ? envelope.seq : 0;
  result.envelope.type = type;

  switch (type) {
    case MessageType::DriveCommand: {
      if (envelope.which_payload !=
          vtsrtc_control_v1_VehicleControlEnvelope_drive_command_tag) {
        return DecodeError(DecodeStatus::UnexpectedPayload,
                           "drive command payload is missing or mismatched");
      }
      const PbDriveCommand& source = envelope.payload.drive_command;
      if (!source.has_drive_direction || !source.has_steering_direction ||
          !source.has_throttle || !source.has_brake ||
          !IsKnownDriveDirection(source.drive_direction) ||
          !IsKnownSteeringDirection(source.steering_direction)) {
        return DecodeError(DecodeStatus::InvalidField,
                           "drive command has missing or unknown fields");
      }
      result.envelope.drive_command.drive_direction =
          FromPbDriveDirection(source.drive_direction);
      result.envelope.drive_command.steering_direction =
          FromPbSteeringDirection(source.steering_direction);
      result.envelope.drive_command.throttle = source.throttle;
      result.envelope.drive_command.brake = source.brake;
      const ValidationResult validation =
          ValidateDriveCommand(result.envelope.drive_command);
      if (!validation) {
        return DecodeError(DecodeStatus::InvalidField, validation.error_message);
      }
      break;
    }
    case MessageType::SetGear: {
      if (envelope.which_payload !=
          vtsrtc_control_v1_VehicleControlEnvelope_set_gear_tag) {
        return DecodeError(DecodeStatus::UnexpectedPayload,
                           "set gear payload is missing or mismatched");
      }
      const PbSetGear& source = envelope.payload.set_gear;
      if (!source.has_request_id || !source.has_gear || !IsKnownGear(source.gear)) {
        return DecodeError(DecodeStatus::InvalidField,
                           "set gear has missing or unknown fields");
      }
      result.envelope.set_gear.request_id = source.request_id;
      result.envelope.set_gear.gear = FromPbGear(source.gear);
      const ValidationResult validation = ValidateSetGear(result.envelope.set_gear);
      if (!validation) {
        return DecodeError(DecodeStatus::InvalidField, validation.error_message);
      }
      break;
    }
    case MessageType::EventAck: {
      if (envelope.which_payload !=
          vtsrtc_control_v1_VehicleControlEnvelope_event_ack_tag) {
        return DecodeError(DecodeStatus::UnexpectedPayload,
                           "event ack payload is missing or mismatched");
      }
      const PbEventAck& source = envelope.payload.event_ack;
      if (!source.has_request_id || !source.has_accepted ||
          !source.has_error_code || !source.has_active_gear ||
          !IsKnownErrorCode(source.error_code) || !IsKnownGear(source.active_gear) ||
          source.request_id == 0) {
        return DecodeError(DecodeStatus::InvalidField,
                           "event ack has missing or unknown fields");
      }
      result.envelope.event_ack.request_id = source.request_id;
      result.envelope.event_ack.accepted = source.accepted;
      result.envelope.event_ack.error_code = FromPbErrorCode(source.error_code);
      result.envelope.event_ack.active_gear = FromPbGear(source.active_gear);
      break;
    }
    case MessageType::VehicleState: {
      if (envelope.which_payload !=
          vtsrtc_control_v1_VehicleControlEnvelope_vehicle_state_tag) {
        return DecodeError(DecodeStatus::UnexpectedPayload,
                           "vehicle state payload is missing or mismatched");
      }
      const PbState& source = envelope.payload.vehicle_state;
      if (!source.has_active_gear || !source.has_last_received_drive_seq ||
          !source.has_watchdog_stopped || !IsKnownGear(source.active_gear)) {
        return DecodeError(DecodeStatus::InvalidField,
                           "vehicle state has missing or unknown fields");
      }
      result.envelope.vehicle_state.active_gear = FromPbGear(source.active_gear);
      result.envelope.vehicle_state.last_received_drive_seq =
          source.last_received_drive_seq;
      result.envelope.vehicle_state.watchdog_stopped = source.watchdog_stopped;
      break;
    }
    case MessageType::Unknown:
    default:
      return DecodeError(DecodeStatus::InvalidEnvelope, "unknown message type");
  }
  return result;
}

DriveCommandGate::DriveCommandGate(uint32_t watchdog_ms)
    : watchdog_ms_(watchdog_ms == 0 ? kDefaultDriveWatchdogMs : watchdog_ms) {}

void DriveCommandGate::Start(uint64_t now_ms) {
  started_ = true;
  watchdog_stopped_ = false;
  has_received_drive_ = false;
  last_received_seq_ = 0;
  last_valid_command_ms_ = now_ms;
}

void DriveCommandGate::Stop() {
  started_ = false;
  watchdog_stopped_ = true;
}

DriveReceiveResult DriveCommandGate::Accept(uint64_t seq,
                                             const DriveCommand& command,
                                             uint64_t now_ms) {
  if (!started_) {
    return {DriveReceiveStatus::NotStarted, true, "drive gate is not started"};
  }
  if (PollWatchdog(now_ms)) {
    return {DriveReceiveStatus::WatchdogExpired, true, "drive watchdog expired"};
  }
  if (has_received_drive_ && seq <= last_received_seq_) {
    return {DriveReceiveStatus::DuplicateOrOutOfOrder, false,
            "drive sequence is duplicate or out of order"};
  }
  const ValidationResult validation = ValidateDriveCommand(command);
  if (!validation) {
    Stop();
    return {DriveReceiveStatus::InvalidCommand, true, validation.error_message};
  }
  has_received_drive_ = true;
  last_received_seq_ = seq;
  last_valid_command_ms_ = now_ms;
  return {DriveReceiveStatus::Accepted, false, ""};
}

bool DriveCommandGate::PollWatchdog(uint64_t now_ms) {
  if (!started_) {
    return watchdog_stopped_;
  }
  if (now_ms >= last_valid_command_ms_ &&
      now_ms - last_valid_command_ms_ >= watchdog_ms_) {
    Stop();
    return true;
  }
  return false;
}

}  // 命名空间 control
}  // 命名空间 vts_rtc
