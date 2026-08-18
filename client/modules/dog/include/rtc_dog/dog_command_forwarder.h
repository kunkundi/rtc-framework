#pragma once

#include "rtc_vehicle/vehicle_control_interface.h"
#include "rtc_vehicle_protocol/vehicle_control_protocol.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace rtc_dog {

// 狗指令转发器。
// 实现 VehicleControlInterface，将车辆控制指令（DriveCommand / SetGear）
// 映射为机器狗的速度指令，通过 WebSocket 转发到 rosbridge
//（默认 ws://10.10.10.10:9090），发布到 ROS 话题
// /alphadog_node/set_velocity。
//
// 内部使用原生 TCP 连接 + 手写 WebSocket 帧，绕过 DNS 解析，
// 避免 Orin NX 上 SimpleWeb 内置 resolver 的配置问题。
// 拥有独立的 asio::io_context 线程，所有 I/O 操作与调用者线程隔离。
//
// DriveCommand → 狗速度映射：
//   throttle × drive_direction → vx (m/s)
//   steering_direction          → wz (rad/s)
//   brake > 0                   → 立即停车 (vx=vy=wz=0)
//
// 使用方式：
//   1. 配置 EdgeOptions::dog_control.enabled = true
//   2. edge_application 将 forwarder 作为 VehicleControlInterface 注入
//      VehicleControlModule
//   3. 远端通过 vehicle.control.v1 通道发送标准车辆控制消息，
//      VehicleControlModule 自动调用 SendDriveCommand/SendStop
class DogCommandForwarder : public rtc_vehicle::VehicleControlInterface {
 public:
  struct Config {
    // rosbridge WebSocket 地址，主机必须使用数值 IP，可包含路径。
    std::string rosbridge_url = "ws://10.10.10.10:9090";
    // 断开后自动重连的间隔，单位毫秒。
    int reconnect_interval_ms = 3000;
    // 油门满量程对应的狗前进速度，单位 m/s。
    float max_forward_speed = 1.0f;
    // 方向盘最大转角对应的狗旋转速度，单位 rad/s。
    float max_angular_speed = 1.0f;
    float max_lateral_speed = 0.5f;
    // 首次连接和单次网络操作的超时时间，单位毫秒。
    int connect_timeout_ms = 3000;
    // 关闭时等待停车帧写出的最长时间，单位毫秒。
    int shutdown_timeout_ms = 250;
  };

  explicit DogCommandForwarder(const Config& config);
  ~DogCommandForwarder() override;

  DogCommandForwarder(const DogCommandForwarder&) = delete;
  DogCommandForwarder& operator=(const DogCommandForwarder&) = delete;

  // ── VehicleControlInterface 实现 ────────────────────────────────────────
  // 调用者线程：主线程。
  bool Open(std::string* error_message) override;
  void Close() override;

  // 将车辆驾驶指令映射为狗速度并通过 WebSocket 转发。
  // 调用者线程：VehicleControlModule::Tick 线程。
  rtc_vehicle::VehicleCommandResult SendDriveCommand(
      const vts_rtc::vehicle::DriveCommand& command) override;

  // 档位切换指令。目前不映射到狗行为，直接接受。
  // 调用者线程：VehicleControlModule::Tick 线程。
  rtc_vehicle::VehicleCommandResult SendGearCommand(
      vts_rtc::vehicle::VehicleGear gear) override;
  rtc_vehicle::VehicleCommandResult SendDogAction(
      const vts_rtc::vehicle::DogAction& action) override;

  // 紧急停车，发送零速度到狗。
  // 调用者线程：VehicleControlModule::Tick 线程。
  void SendStop() override;

  // ── 生命周期 ────────────────────────────────────────────────────────────

  // 查询 WebSocket 连接是否已建立。
  bool IsConnected() const;

 private:
  // 启动 io_context 线程并发起首次 WebSocket 连接。
  bool Start(std::string* error_message);

  // 安全释放：释放 work_guard → 停 io_context → join 线程 → 清理 socket。
  void StopImpl();

  // 将解码后的车辆指令编码为 rosbridge JSON 并通过 WebSocket 发送。
  // 内置节流：相同速度指令 50ms 内不重复发送，stop 始终立即转发。
  bool ForwardVelocity(float vx, float vy, float wz);

  // 将站立/趴下编码为 actionlib goal，通过 WebSocket 发布到
  // /agent_skill/do_dog_behavior/execute/goal。goal_id 追加时间戳+随机数
  // 保证每次唯一，stamp 用当前时间、header.seq 递增，与实测可用的
  // 键盘控制脚本一致。
  bool ForwardDogBehaviorGoal(const char* goal_id, const char* args);

  // PIMPL：隐藏所有 asio/WebSocket 实现细节，避免头文件污染。
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace rtc_dog
