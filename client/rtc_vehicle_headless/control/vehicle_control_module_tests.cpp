#include "vehicle_control_module.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

using vts_rtc::control::DecodeEnvelope;
using vts_rtc::control::DriveCommand;
using vts_rtc::control::DriveDirection;
using vts_rtc::control::EncodeDriveCommand;
using vts_rtc::control::EncodeSetGear;
using vts_rtc::control::MessageType;
using vts_rtc::control::SetGear;
using vts_rtc::control::VehicleErrorCode;
using vts_rtc::control::VehicleGear;

void Check(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << std::endl;
    std::exit(1);
  }
}

class FakeIndustrialControl final
    : public rtc_vehicle::IndustrialControlInterface {
 public:
  bool Open(std::string* error_message) override {
    opened = true;
    if (error_message) {
      error_message->clear();
    }
    return true;
  }

  void Close() override { opened = false; }

  rtc_vehicle::IndustrialCommandResult SendDriveCommand(
      const DriveCommand& command) override {
    ++drive_count;
    last_drive = command;
    return Accepted();
  }

  rtc_vehicle::IndustrialCommandResult SendGearCommand(
      VehicleGear gear) override {
    ++gear_count;
    last_gear = gear;
    return Accepted();
  }

  void SendStop() override { ++stop_count; }

  static rtc_vehicle::IndustrialCommandResult Accepted() {
    rtc_vehicle::IndustrialCommandResult result;
    result.accepted = true;
    result.error_code = VehicleErrorCode::None;
    return result;
  }

  bool opened = false;
  int drive_count = 0;
  int gear_count = 0;
  int stop_count = 0;
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
                    const vts_rtc::control::EncodeResult& encoded) {
  Check(static_cast<bool>(encoded), "编码测试消息");
  module->EnqueueMessage(remote_sessionid, label,
                         reinterpret_cast<const char*>(encoded.payload.data()),
                         encoded.payload.size());
}

void TestControlFlow() {
  FakeIndustrialControl industrial;
  std::vector<SentPacket> sent_packets;
  std::vector<std::string> errors;

  rtc_vehicle::VehicleControlModule module(
      &industrial,
      [&sent_packets](RtcSessionId remote_sessionid, const char* label,
                      const std::vector<uint8_t>& payload) {
        sent_packets.push_back({remote_sessionid, label, payload});
        return true;
      },
      [](const std::string&) {},
      [&errors](const std::string& message) { errors.push_back(message); });

  std::string start_error;
  Check(module.Start(&start_error), "启动控制模块");
  Check(industrial.opened, "打开工控机接口");

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
  Check(industrial.stop_count == 0, "首条驾驶帧前不启动看门狗");

  DriveCommand drive;
  drive.drive_direction = DriveDirection::Forward;
  drive.throttle = 0.5f;
  const auto encoded_drive = EncodeDriveCommand(10, drive);
  EnqueueEncoded(&module, 7,
                 vts_rtc::control::kVehicleControlChannelLabel,
                 encoded_drive);
  module.Tick(1410);
  Check(industrial.drive_count == 1, "下发驾驶指令");
  Check(industrial.last_drive.drive_direction == DriveDirection::Forward,
        "保留驾驶方向");

  EnqueueEncoded(&module, 7,
                 vts_rtc::control::kVehicleControlChannelLabel,
                 encoded_drive);
  module.Tick(1420);
  Check(industrial.drive_count == 1, "丢弃重复驾驶序号");

  SetGear set_gear;
  set_gear.request_id = 99;
  set_gear.gear = VehicleGear::Reverse;
  EnqueueEncoded(&module, 7, vts_rtc::control::kVehicleEventChannelLabel,
                 EncodeSetGear(11, set_gear));
  module.Tick(1430);
  Check(industrial.gear_count == 1, "下发档位指令");
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
  Check(industrial.stop_count == 1, "看门狗超时下发停车");

  DriveCommand stopped_drive;
  stopped_drive.drive_direction = DriveDirection::Forward;
  EnqueueEncoded(&module, 7,
                 vts_rtc::control::kVehicleControlChannelLabel,
                 EncodeDriveCommand(12, stopped_drive));
  module.Tick(1720);
  Check(industrial.drive_count == 1, "安全停车后保持驾驶门控锁止");

  SetGear locked_gear;
  locked_gear.request_id = 100;
  locked_gear.gear = VehicleGear::Forward;
  EnqueueEncoded(&module, 7, vts_rtc::control::kVehicleEventChannelLabel,
                 EncodeSetGear(13, locked_gear));
  module.Tick(1725);
  Check(industrial.gear_count == 1, "安全停车后拒绝换档下发");

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
  Check(industrial.stop_count == 1, "安全停车只下发一次");

  module.Shutdown();
  Check(!industrial.opened, "关闭工控机接口");
}

void TestTransportDisconnectStopsVehicle() {
  FakeIndustrialControl industrial;
  rtc_vehicle::VehicleControlModule module(
      &industrial,
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
  Check(industrial.stop_count == 1, "服务端链路断开触发停车");
}

void TestWatchdogUpperBound() {
  FakeIndustrialControl industrial;
  rtc_vehicle::VehicleControlModuleOptions options;
  options.watchdog_ms = vts_rtc::control::kDefaultDriveWatchdogMs + 1;
  rtc_vehicle::VehicleControlModule module(
      &industrial,
      [](RtcSessionId, const char*, const std::vector<uint8_t>&) {
        return true;
      },
      [](const std::string&) {}, [](const std::string&) {}, options);
  std::string error;
  Check(!module.Start(&error), "拒绝超过协议上限的看门狗配置");
  Check(!industrial.opened, "非法看门狗配置不得打开工控机接口");
}

void TestMalformedMessageStopsVehicle() {
  FakeIndustrialControl industrial;
  rtc_vehicle::VehicleControlModule module(
      &industrial,
      [](RtcSessionId, const char*, const std::vector<uint8_t>&) {
        return true;
      },
      [](const std::string&) {}, [](const std::string&) {});
  std::string error;
  Check(module.Start(&error), "启动畸形消息测试模块");
  module.EnqueueP2PState(8, P2PConnected);
  module.Tick(2000);

  const char malformed[] = {1, 2, 3};
  module.EnqueueMessage(8, vts_rtc::control::kVehicleControlChannelLabel,
                        malformed, sizeof(malformed));
  module.Tick(2010);
  Check(industrial.stop_count == 1, "畸形实时消息触发停车");
}

void TestInvalidPayloadBoundaryStopsVehicle() {
  {
    FakeIndustrialControl industrial;
    rtc_vehicle::VehicleControlModule module(
        &industrial,
        [](RtcSessionId, const char*, const std::vector<uint8_t>&) {
          return true;
        },
        [](const std::string&) {}, [](const std::string&) {});
    std::string error;
    Check(module.Start(&error), "启动空载荷测试模块");
    module.EnqueueP2PState(10, P2PConnected);
    module.Tick(4000);
    module.EnqueueMessage(10, vts_rtc::control::kVehicleControlChannelLabel,
                          nullptr, 0);
    module.Tick(4010);
    Check(industrial.stop_count == 1, "空控制载荷触发停车");
  }

  {
    FakeIndustrialControl industrial;
    rtc_vehicle::VehicleControlModule module(
        &industrial,
        [](RtcSessionId, const char*, const std::vector<uint8_t>&) {
          return true;
        },
        [](const std::string&) {}, [](const std::string&) {});
    std::string error;
    Check(module.Start(&error), "启动超限载荷测试模块");
    module.EnqueueP2PState(11, P2PConnected);
    module.Tick(5000);
    const std::vector<char> oversized(4097, 1);
    module.EnqueueMessage(11, vts_rtc::control::kVehicleControlChannelLabel,
                          oversized.data(), oversized.size());
    module.Tick(5010);
    Check(industrial.stop_count == 1, "超限控制载荷触发停车");
  }
}

}  // 匿名命名空间

int main() {
  TestControlFlow();
  TestMalformedMessageStopsVehicle();
  TestInvalidPayloadBoundaryStopsVehicle();
  TestTransportDisconnectStopsVehicle();
  TestWatchdogUpperBound();
  std::cout << "rtc_vehicle_control_module_tests passed" << std::endl;
  return 0;
}
