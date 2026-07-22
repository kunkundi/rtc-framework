#include "rtc_vehicle/vehicle_control_module.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

using vts_rtc::vehicle::DecodeEnvelope;
using vts_rtc::vehicle::DriveCommand;
using vts_rtc::vehicle::DriveDirection;
using vts_rtc::vehicle::EncodeDriveCommand;
using vts_rtc::vehicle::EncodeSetGear;
using vts_rtc::vehicle::MessageType;
using vts_rtc::vehicle::SetGear;
using vts_rtc::vehicle::VehicleErrorCode;
using vts_rtc::vehicle::VehicleGear;

void Check(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << std::endl;
    std::exit(1);
  }
}

class FakeVehicleControl final
    : public rtc_vehicle::VehicleControlInterface {
 public:
  bool Open(std::string* error_message) override {
    opened = true;
    if (error_message) {
      error_message->clear();
    }
    return true;
  }

  void Close() override { opened = false; }

  rtc_vehicle::VehicleCommandResult SendDriveCommand(
      const DriveCommand& command) override {
    ++drive_count;
    last_drive = command;
    if (reject_drive_as_unavailable) {
      rtc_vehicle::VehicleCommandResult result;
      result.accepted = false;
      result.error_code = VehicleErrorCode::InvalidState;
      result.detail = "test interface unavailable";
      return result;
    }
    return Accepted();
  }

  rtc_vehicle::VehicleCommandResult SendGearCommand(
      VehicleGear gear) override {
    ++gear_count;
    last_gear = gear;
    return Accepted();
  }

  void SendStop() override { ++stop_count; }

  static rtc_vehicle::VehicleCommandResult Accepted() {
    rtc_vehicle::VehicleCommandResult result;
    result.accepted = true;
    result.error_code = VehicleErrorCode::None;
    return result;
  }

  bool opened = false;
  int drive_count = 0;
  int gear_count = 0;
  int stop_count = 0;
  bool reject_drive_as_unavailable = false;
  DriveCommand last_drive;
  VehicleGear last_gear = VehicleGear::Neutral;
};

struct SentPacket {
  RtcSessionId remote_sessionid = 0;
  std::string label;
  std::vector<uint8_t> payload;
};

void EnqueueEncoded(rtc_vehicle::VehicleControlModule* module,
                    RtcSessionId remote_sessionid,
                    const char* label,
                    const vts_rtc::vehicle::EncodeResult& encoded) {
  Check(static_cast<bool>(encoded), "编码测试消息");
  module->EnqueueMessage(remote_sessionid, label,
                         reinterpret_cast<const char*>(encoded.payload.data()),
                         encoded.payload.size());
}

void TestControlFlow() {
  FakeVehicleControl vehicle;
  std::vector<SentPacket> sent_packets;
  std::vector<std::string> errors;

  rtc_vehicle::VehicleControlModule module(
      &vehicle,
      [&sent_packets](RtcSessionId remote_sessionid, const char* label,
                      const std::vector<uint8_t>& payload) {
        sent_packets.push_back({remote_sessionid, label, payload});
        return true;
      },
      [](const std::string&) {},
      [&errors](const std::string& message) { errors.push_back(message); });

  std::string start_error;
  Check(module.Start(&start_error), "启动控制模块");
  Check(vehicle.opened, "打开车辆控制接口");

  module.EnqueueP2PState(7, P2PConnected);
  module.Tick(1000);
  Check(module.has_active_peer(), "记录活动控制端");
  Check(!sent_packets.empty(), "连接后立即发送车辆状态");
  const auto initial_state = DecodeEnvelope(sent_packets.back().payload);
  Check(initial_state &&
            initial_state.envelope.type == MessageType::VehicleState &&
            !initial_state.envelope.vehicle_state.watchdog_stopped,
        "等待首条驾驶帧时不误报看门狗停车");
  sent_packets.clear();

  module.Tick(1400);
  Check(vehicle.stop_count == 0, "首条驾驶帧前不启动看门狗");

  DriveCommand drive;
  drive.drive_direction = DriveDirection::Forward;
  drive.throttle = 0.5f;
  const auto encoded_drive = EncodeDriveCommand(10, drive);
  EnqueueEncoded(&module, 7,
                 vts_rtc::vehicle::kVehicleControlChannelLabel,
                 encoded_drive);
  module.Tick(1410);
  Check(vehicle.drive_count == 1, "下发驾驶指令");
  Check(vehicle.last_drive.drive_direction == DriveDirection::Forward,
        "保留驾驶方向");

  EnqueueEncoded(&module, 7,
                 vts_rtc::vehicle::kVehicleControlChannelLabel,
                 encoded_drive);
  module.Tick(1420);
  Check(vehicle.drive_count == 1, "丢弃重复驾驶序号");

  SetGear set_gear;
  set_gear.request_id = 99;
  set_gear.gear = VehicleGear::Reverse;
  EnqueueEncoded(&module, 7, vts_rtc::vehicle::kVehicleEventChannelLabel,
                 EncodeSetGear(11, set_gear));
  module.Tick(1430);
  Check(vehicle.gear_count == 1, "下发档位指令");
  Check(module.active_gear() == VehicleGear::Reverse, "更新活动档位");

  bool ack_seen = false;
  for (const SentPacket& packet : sent_packets) {
    const auto decoded = DecodeEnvelope(packet.payload);
    if (decoded && decoded.envelope.type == MessageType::EventAck) {
      ack_seen = decoded.envelope.event_ack.request_id == 99 &&
                 decoded.envelope.event_ack.accepted &&
                 decoded.envelope.event_ack.active_gear ==
                     VehicleGear::Reverse;
    }
  }
  Check(ack_seen, "返回档位事务回执");

  module.Tick(1710);
  Check(vehicle.stop_count == 1, "看门狗超时下发停车");

  DriveCommand stopped_drive;
  stopped_drive.drive_direction = DriveDirection::Forward;
  EnqueueEncoded(&module, 7,
                 vts_rtc::vehicle::kVehicleControlChannelLabel,
                 EncodeDriveCommand(12, stopped_drive));
  module.Tick(1720);
  Check(vehicle.drive_count == 1, "安全停车后保持驾驶门控锁止");

  SetGear locked_gear;
  locked_gear.request_id = 100;
  locked_gear.gear = VehicleGear::Forward;
  EnqueueEncoded(&module, 7, vts_rtc::vehicle::kVehicleEventChannelLabel,
                 EncodeSetGear(13, locked_gear));
  module.Tick(1725);
  Check(vehicle.gear_count == 1, "安全停车后拒绝换档下发");

  bool locked_ack_seen = false;
  for (const SentPacket& packet : sent_packets) {
    const auto decoded = DecodeEnvelope(packet.payload);
    if (decoded && decoded.envelope.type == MessageType::EventAck &&
        decoded.envelope.event_ack.request_id == 100) {
      locked_ack_seen = !decoded.envelope.event_ack.accepted &&
                        decoded.envelope.event_ack.error_code ==
                            VehicleErrorCode::InvalidState;
    }
  }
  Check(locked_ack_seen, "安全停车后返回拒绝换档回执");

  module.EnqueueP2PState(7, P2PDisconnected);
  module.Tick(1730);
  Check(!module.has_active_peer(), "断线后清除活动控制端");
  Check(vehicle.stop_count == 1, "安全停车只下发一次");

  module.Shutdown();
  Check(!vehicle.opened, "关闭车辆控制接口");
}

void TestTransportDisconnectStopsVehicle() {
  FakeVehicleControl vehicle;
  rtc_vehicle::VehicleControlModule module(
      &vehicle,
      [](RtcSessionId, const char*, const std::vector<uint8_t>&) {
        return true;
      },
      [](const std::string&) {}, [](const std::string&) {});
  std::string error;
  Check(module.Start(&error), "启动传输断线测试模块");
  module.EnqueueP2PState(9, P2PConnected);
  module.Tick(3000);
  module.EnqueueTransportDisconnected();
  module.Tick(3010);
  Check(!module.has_active_peer(), "服务端链路断开后清除活动控制端");
  Check(vehicle.stop_count == 1, "服务端链路断开触发停车");
}

void TestWatchdogUpperBound() {
  FakeVehicleControl vehicle;
  rtc_vehicle::VehicleControlModuleOptions options;
  options.watchdog_ms = vts_rtc::vehicle::kDefaultDriveWatchdogMs + 1;
  rtc_vehicle::VehicleControlModule module(
      &vehicle,
      [](RtcSessionId, const char*, const std::vector<uint8_t>&) {
        return true;
      },
      [](const std::string&) {}, [](const std::string&) {}, options);
  std::string error;
  Check(!module.Start(&error), "拒绝超过协议上限的看门狗配置");
  Check(!vehicle.opened, "非法看门狗配置不得打开车辆控制接口");
}

void TestWatchdogRecoveryPreservesSequenceAndState() {
  FakeVehicleControl vehicle;
  std::vector<SentPacket> sent_packets;
  rtc_vehicle::VehicleControlModuleOptions options;
  options.allow_watchdog_recovery = true;
  rtc_vehicle::VehicleControlModule module(
      &vehicle,
      [&sent_packets](RtcSessionId remote_sessionid, const char* label,
                      const std::vector<uint8_t>& payload) {
        sent_packets.push_back({remote_sessionid, label, payload});
        return true;
      },
      [](const std::string&) {}, [](const std::string&) {}, options);

  std::string error;
  Check(module.Start(&error), "启动看门狗恢复测试模块");
  module.EnqueueP2PState(12, P2PConnected);
  module.Tick(6000);

  DriveCommand drive;
  drive.drive_direction = DriveDirection::Forward;
  drive.throttle = 0.5f;
  EnqueueEncoded(&module, 12,
                 vts_rtc::vehicle::kVehicleControlChannelLabel,
                 EncodeDriveCommand(20, drive));
  module.Tick(6010);
  Check(vehicle.drive_count == 1, "恢复测试先接受初始驾驶帧");

  module.Tick(6310);
  Check(vehicle.stop_count == 1, "恢复模式看门狗超时仍然停车");
  Check(!sent_packets.empty(), "看门狗停车后上报状态");
  const auto stopped_state = DecodeEnvelope(sent_packets.back().payload);
  Check(stopped_state &&
            stopped_state.envelope.type == MessageType::VehicleState &&
            stopped_state.envelope.vehicle_state.watchdog_stopped,
        "恢复等待期间上报看门狗已停车");

  EnqueueEncoded(&module, 12,
                 vts_rtc::vehicle::kVehicleControlChannelLabel,
                 EncodeDriveCommand(19, drive));
  module.Tick(6320);
  Check(vehicle.drive_count == 1, "恢复时拒绝延迟到达的旧序号");

  EnqueueEncoded(&module, 12,
                 vts_rtc::vehicle::kVehicleControlChannelLabel,
                 EncodeDriveCommand(21, drive));
  module.Tick(6330);
  Check(vehicle.drive_count == 2, "恢复时接受单调递增的新序号");
  Check(!sent_packets.empty(), "恢复后上报状态");
  const auto recovered_state = DecodeEnvelope(sent_packets.back().payload);
  Check(recovered_state &&
            recovered_state.envelope.type == MessageType::VehicleState &&
            !recovered_state.envelope.vehicle_state.watchdog_stopped &&
            recovered_state.envelope.vehicle_state.last_received_drive_seq ==
                21,
        "合法新帧接受后清除看门狗停车状态");
}

void TestInterfaceRecoveryRequiresNewSequence() {
  FakeVehicleControl vehicle;
  std::vector<SentPacket> sent_packets;
  rtc_vehicle::VehicleControlModuleOptions options;
  options.allow_interface_recovery = true;
  rtc_vehicle::VehicleControlModule module(
      &vehicle,
      [&sent_packets](RtcSessionId remote_sessionid, const char* label,
                      const std::vector<uint8_t>& payload) {
        sent_packets.push_back({remote_sessionid, label, payload});
        return true;
      },
      [](const std::string&) {}, [](const std::string&) {}, options);

  std::string error;
  Check(module.Start(&error), "启动接口恢复测试模块");
  module.EnqueueP2PState(13, P2PConnected);
  module.Tick(7000);

  DriveCommand drive;
  drive.drive_direction = DriveDirection::Forward;
  drive.throttle = 0.5f;
  vehicle.reject_drive_as_unavailable = true;
  EnqueueEncoded(&module, 13,
                 vts_rtc::vehicle::kVehicleControlChannelLabel,
                 EncodeDriveCommand(30, drive));
  module.Tick(7010);
  Check(vehicle.stop_count == 1, "接口不可用时触发停车");
  const auto unavailable_state = DecodeEnvelope(sent_packets.back().payload);
  Check(unavailable_state &&
            unavailable_state.envelope.type == MessageType::VehicleState &&
            unavailable_state.envelope.vehicle_state.watchdog_stopped,
        "接口不可用期间上报停车状态");

  vehicle.reject_drive_as_unavailable = false;
  EnqueueEncoded(&module, 13,
                 vts_rtc::vehicle::kVehicleControlChannelLabel,
                 EncodeDriveCommand(30, drive));
  module.Tick(7020);
  Check(vehicle.drive_count == 1, "接口恢复时仍拒绝重复序号");

  EnqueueEncoded(&module, 13,
                 vts_rtc::vehicle::kVehicleControlChannelLabel,
                 EncodeDriveCommand(31, drive));
  module.Tick(7030);
  Check(vehicle.drive_count == 2, "接口恢复后接受新序号");
  const auto recovered_state = DecodeEnvelope(sent_packets.back().payload);
  Check(recovered_state &&
            recovered_state.envelope.type == MessageType::VehicleState &&
            !recovered_state.envelope.vehicle_state.watchdog_stopped,
        "接口恢复后清除停车状态");
}

void TestMalformedMessageStopsVehicle() {
  FakeVehicleControl vehicle;
  rtc_vehicle::VehicleControlModule module(
      &vehicle,
      [](RtcSessionId, const char*, const std::vector<uint8_t>&) {
        return true;
      },
      [](const std::string&) {}, [](const std::string&) {});
  std::string error;
  Check(module.Start(&error), "启动畸形消息测试模块");
  module.EnqueueP2PState(8, P2PConnected);
  module.Tick(2000);

  const char malformed[] = {1, 2, 3};
  module.EnqueueMessage(8, vts_rtc::vehicle::kVehicleControlChannelLabel,
                        malformed, sizeof(malformed));
  module.Tick(2010);
  Check(vehicle.stop_count == 1, "畸形实时消息触发停车");
}

void TestInvalidPayloadBoundaryStopsVehicle() {
  {
    FakeVehicleControl vehicle;
    rtc_vehicle::VehicleControlModule module(
        &vehicle,
        [](RtcSessionId, const char*, const std::vector<uint8_t>&) {
          return true;
        },
        [](const std::string&) {}, [](const std::string&) {});
    std::string error;
    Check(module.Start(&error), "启动空载荷测试模块");
    module.EnqueueP2PState(10, P2PConnected);
    module.Tick(4000);
    module.EnqueueMessage(10, vts_rtc::vehicle::kVehicleControlChannelLabel,
                          nullptr, 0);
    module.Tick(4010);
    Check(vehicle.stop_count == 1, "空控制载荷触发停车");
  }

  {
    FakeVehicleControl vehicle;
    rtc_vehicle::VehicleControlModule module(
        &vehicle,
        [](RtcSessionId, const char*, const std::vector<uint8_t>&) {
          return true;
        },
        [](const std::string&) {}, [](const std::string&) {});
    std::string error;
    Check(module.Start(&error), "启动超限载荷测试模块");
    module.EnqueueP2PState(11, P2PConnected);
    module.Tick(5000);
    const std::vector<char> oversized(4097, 1);
    module.EnqueueMessage(11, vts_rtc::vehicle::kVehicleControlChannelLabel,
                          oversized.data(), oversized.size());
    module.Tick(5010);
    Check(vehicle.stop_count == 1, "超限控制载荷触发停车");
  }
}

}  // 匿名命名空间

int main() {
  TestControlFlow();
  TestMalformedMessageStopsVehicle();
  TestInvalidPayloadBoundaryStopsVehicle();
  TestTransportDisconnectStopsVehicle();
  TestWatchdogUpperBound();
  TestWatchdogRecoveryPreservesSequenceAndState();
  TestInterfaceRecoveryRequiresNewSequence();
  std::cout << "rtc_vehicle_control_module_tests passed" << std::endl;
  return 0;
}
