#include "rtc_control/vehicle_control_protocol.h"

#include "rtc_control.pb.h"

#include <pb_encode.h>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {

using vts_rtc::control::DecodeEnvelope;
using vts_rtc::control::DecodeStatus;
using vts_rtc::control::DriveCommand;
using vts_rtc::control::DriveCommandGate;
using vts_rtc::control::DriveDirection;
using vts_rtc::control::DriveReceiveStatus;
using vts_rtc::control::EncodeDriveCommand;
using vts_rtc::control::EncodeEventAck;
using vts_rtc::control::EncodeSetGear;
using vts_rtc::control::EncodeVehicleState;
using vts_rtc::control::EventAck;
using vts_rtc::control::MessageType;
using vts_rtc::control::SetGear;
using vts_rtc::control::SteeringDirection;
using vts_rtc::control::VehicleErrorCode;
using vts_rtc::control::VehicleGear;
using vts_rtc::control::VehicleState;

void Check(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << std::endl;
    std::exit(1);
  }
}

std::vector<uint8_t> EncodeRaw(
    const vtsrtc_control_v1_VehicleControlEnvelope& envelope) {
  std::vector<uint8_t> bytes(
      vtsrtc_control_v1_VehicleControlEnvelope_size);
  pb_ostream_t stream = pb_ostream_from_buffer(bytes.data(), bytes.size());
  Check(pb_encode(&stream, vtsrtc_control_v1_VehicleControlEnvelope_fields,
                  &envelope),
        "encode raw nanopb envelope");
  bytes.resize(stream.bytes_written);
  return bytes;
}

void FillHeader(vtsrtc_control_v1_VehicleControlEnvelope* envelope,
                vtsrtc_control_v1_VehicleMessageType type) {
  envelope->has_magic = true;
  envelope->magic = vts_rtc::control::kVehicleControlMagic;
  envelope->has_protocol_major = true;
  envelope->protocol_major = vts_rtc::control::kVehicleControlProtocolMajor;
  envelope->has_protocol_minor = true;
  envelope->protocol_minor = vts_rtc::control::kVehicleControlProtocolMinor;
  envelope->has_type = true;
  envelope->type = type;
  envelope->has_seq = true;
  envelope->seq = 1;
}

void TestDriveRoundTripAndGolden() {
  DriveCommand command;
  command.drive_direction = DriveDirection::Forward;
  command.steering_direction = SteeringDirection::Right;
  command.throttle = 0.5f;
  command.brake = 0.25f;

  const auto encoded = EncodeDriveCommand(42, command);
  Check(static_cast<bool>(encoded), "encode drive command");

  // 该消息在 v1 协议中的固定 protobuf 字节。
  const std::vector<uint8_t> golden = {
      0x08, 0xd6, 0x86, 0xb1, 0x8a, 0x03, 0x10, 0x01, 0x18, 0x00,
      0x20, 0x01, 0x28, 0x2a, 0x52, 0x0e, 0x08, 0x01, 0x10, 0x02,
      0x1d, 0x00, 0x00, 0x00, 0x3f, 0x25, 0x00, 0x00, 0x80, 0x3e,
  };
  Check(encoded.payload == golden, "drive command golden vector");

  const auto decoded = DecodeEnvelope(encoded.payload);
  Check(static_cast<bool>(decoded), "decode drive command");
  Check(decoded.envelope.type == MessageType::DriveCommand,
        "decoded drive type");
  Check(decoded.envelope.seq == 42, "decoded drive sequence");
  Check(decoded.envelope.drive_command.drive_direction == DriveDirection::Forward,
        "decoded drive direction");
  Check(decoded.envelope.drive_command.steering_direction == SteeringDirection::Right,
        "decoded steering direction");
  Check(decoded.envelope.drive_command.throttle == 0.5f,
        "decoded throttle");
  Check(decoded.envelope.drive_command.brake == 0.25f, "decoded brake");
}

void TestOtherMessagesRoundTrip() {
  const auto set_gear = EncodeSetGear(5, {99, VehicleGear::Reverse});
  Check(static_cast<bool>(set_gear), "encode set gear");
  const auto decoded_set_gear = DecodeEnvelope(set_gear.payload);
  Check(static_cast<bool>(decoded_set_gear), "decode set gear");
  Check(decoded_set_gear.envelope.type == MessageType::SetGear,
        "decoded set gear type");
  Check(decoded_set_gear.envelope.set_gear.request_id == 99,
        "decoded gear request id");
  Check(decoded_set_gear.envelope.set_gear.gear == VehicleGear::Reverse,
        "decoded gear");

  EventAck ack;
  ack.request_id = 99;
  ack.accepted = true;
  ack.error_code = VehicleErrorCode::None;
  ack.active_gear = VehicleGear::Reverse;
  const auto encoded_ack = EncodeEventAck(6, ack);
  Check(static_cast<bool>(encoded_ack), "encode event ack");
  const auto decoded_ack = DecodeEnvelope(encoded_ack.payload);
  Check(static_cast<bool>(decoded_ack), "decode event ack");
  Check(decoded_ack.envelope.event_ack.accepted, "decoded event ack");

  VehicleState state;
  state.active_gear = VehicleGear::Reverse;
  state.last_received_drive_seq = 42;
  state.watchdog_stopped = false;
  const auto encoded_state = EncodeVehicleState(7, state);
  Check(static_cast<bool>(encoded_state), "encode vehicle state");
  const auto decoded_state = DecodeEnvelope(encoded_state.payload);
  Check(static_cast<bool>(decoded_state), "decode vehicle state");
  Check(decoded_state.envelope.vehicle_state.last_received_drive_seq == 42,
        "decoded vehicle state sequence");
}

void TestRejectedPayloads() {
  DriveCommand invalid;
  invalid.throttle = 1.1f;
  Check(!EncodeDriveCommand(1, invalid), "reject throttle above one");
  invalid.throttle = std::numeric_limits<float>::quiet_NaN();
  Check(!EncodeDriveCommand(1, invalid), "reject NaN throttle");
  invalid.throttle = 0.0f;
  invalid.drive_direction = static_cast<DriveDirection>(99);
  Check(!EncodeDriveCommand(1, invalid), "reject unknown drive direction");
  Check(!EncodeSetGear(1, {0, VehicleGear::Forward}),
        "reject zero gear request id");
  Check(!EncodeSetGear(1, {1, static_cast<VehicleGear>(99)}),
        "reject unknown gear");

  vtsrtc_control_v1_VehicleControlEnvelope bad_magic =
      vtsrtc_control_v1_VehicleControlEnvelope_init_zero;
  FillHeader(&bad_magic,
             vtsrtc_control_v1_VehicleMessageType_VEHICLE_MESSAGE_TYPE_DRIVE_COMMAND);
  bad_magic.magic = 1;
  bad_magic.which_payload =
      vtsrtc_control_v1_VehicleControlEnvelope_drive_command_tag;
  bad_magic.payload.drive_command.has_drive_direction = true;
  bad_magic.payload.drive_command.drive_direction =
      vtsrtc_control_v1_DriveDirection_DRIVE_DIRECTION_STOP;
  bad_magic.payload.drive_command.has_steering_direction = true;
  bad_magic.payload.drive_command.steering_direction =
      vtsrtc_control_v1_SteeringDirection_STEERING_DIRECTION_CENTER;
  bad_magic.payload.drive_command.has_throttle = true;
  bad_magic.payload.drive_command.has_brake = true;
  Check(DecodeEnvelope(EncodeRaw(bad_magic)).status == DecodeStatus::InvalidEnvelope,
        "reject invalid magic");

  vtsrtc_control_v1_VehicleControlEnvelope wrong_payload =
      vtsrtc_control_v1_VehicleControlEnvelope_init_zero;
  FillHeader(&wrong_payload,
             vtsrtc_control_v1_VehicleMessageType_VEHICLE_MESSAGE_TYPE_DRIVE_COMMAND);
  wrong_payload.which_payload =
      vtsrtc_control_v1_VehicleControlEnvelope_set_gear_tag;
  wrong_payload.payload.set_gear.has_request_id = true;
  wrong_payload.payload.set_gear.request_id = 1;
  wrong_payload.payload.set_gear.has_gear = true;
  wrong_payload.payload.set_gear.gear =
      vtsrtc_control_v1_VehicleGear_VEHICLE_GEAR_NEUTRAL;
  Check(DecodeEnvelope(EncodeRaw(wrong_payload)).status ==
            DecodeStatus::UnexpectedPayload,
        "reject mismatched payload");
}

void TestDriveGate() {
  DriveCommand command;
  command.drive_direction = DriveDirection::Forward;
  command.throttle = 0.5f;

  DriveCommandGate gate(300);
  gate.Start(1000);
  Check(gate.Accept(10, command, 1010).status == DriveReceiveStatus::Accepted,
        "accept first drive command");
  Check(gate.Accept(10, command, 1020).status ==
            DriveReceiveStatus::DuplicateOrOutOfOrder,
        "reject duplicate without refreshing watchdog");
  Check(!gate.PollWatchdog(1309), "watchdog remains active before deadline");
  Check(gate.PollWatchdog(1310), "watchdog stops at deadline");

  gate.Start(2000);
  command.brake = std::numeric_limits<float>::infinity();
  const auto invalid = gate.Accept(11, command, 2010);
  Check(invalid.status == DriveReceiveStatus::InvalidCommand && invalid.should_stop,
        "invalid command requests stop");
  Check(gate.watchdog_stopped(), "invalid command leaves gate stopped");
}

}  // 匿名命名空间

int main() {
  TestDriveRoundTripAndGolden();
  TestOtherMessagesRoundTrip();
  TestRejectedPayloads();
  TestDriveGate();
  std::cout << "rtc_vehicle_control_protocol_tests passed" << std::endl;
  return 0;
}
