# rtc_vehicle_headless

`rtc_vehicle_headless` 是车辆端主程序，统一编排 RTC、双目摄像头采集和车辆控制接收。当前仅在 Linux/Jetson 构建。

## 模块结构

- `app/main.cpp`：解析参数，创建各子模块，统一管理启动、循环和关闭顺序。
- `camera/vehicle_camera_module.*`：复用 `rtc_dual_camera_image_source` 完成双摄像头异步采集、I420 转换和 RTC 视频发送。
- `control/vehicle_control_module.*`：在 RTC 回调中复制并排队控制消息，在主线程完成解码、序号校验、看门狗、工控机下发、档位回执和状态发送。
- `control/industrial_control_interface.*`：工控机通信抽象和默认占位实现。

控制数据流：

```text
RTC DataChannel
    -> VehicleControlModule 消息队列
    -> 协议解码、序号与看门狗校验
    -> IndustrialControlInterface
    -> 工控机/CAN/串口/TCP/厂商 SDK
```

摄像头数据流：

```text
双目摄像头
    -> AsyncDualCameraImageSource
    -> VehicleCameraModule
    -> I420 拼接帧
    -> RtcHeadlessSession
```

## 工控机接口

当前主程序使用 `PlaceholderIndustrialControlInterface`。该实现允许摄像头和 RTC 正常运行，但会拒绝所有驾驶及换档指令，并明确返回“工控机接口尚未接入”，不会假装设备已经执行。

接入真实工控机时，实现 `IndustrialControlInterface`：

```cpp
class IndustrialControlInterface {
 public:
  virtual bool Open(std::string* error_message) = 0;
  virtual void Close() = 0;
  virtual IndustrialCommandResult SendDriveCommand(
      const vts_rtc::control::DriveCommand& command) = 0;
  virtual IndustrialCommandResult SendGearCommand(
      vts_rtc::control::VehicleGear gear) = 0;
  virtual void SendStop() = 0;
};
```

然后在 `app/main.cpp` 中将占位实现替换为具体实现。所有方法都在车辆主线程调用，可以在实现中接入 CAN、串口、TCP 或厂商 SDK。`SendStop()` 必须实现为本地安全停车操作。

## 构建和运行

Linux/Jetson 需要现有双摄像头依赖和 CUDA 运行环境：

```sh
xmake build rtc_vehicle_headless
xmake run rtc_vehicle_headless -- \
  --left-device /dev/video0 \
  --right-device /dev/video1 \
  --room zhejianglab
```

常用控制参数：

- `--control-watchdog-ms 300`：驾驶指令看门狗时间，不能超过协议规定的 300 ms 上限。看门狗从首条有效驾驶帧开始计时。
- `--control-state-interval-ms 50`：车辆状态发送周期。

协议错误、看门狗超时、控制队列溢出、RTC 链路断开或工控机拒绝指令后，控制模块会锁存安全停车。本次 P2P 连接中的后续驾驶和换档命令不会解除锁停，必须重新建立控制连接。
单条控制消息的接收上限为 4 KiB，空载荷、无效指针和超限载荷均按协议错误处理。

控制模块单测不依赖摄像头或工控机，可在 Windows/Linux 单独运行：

```sh
xmake build rtc_vehicle_control_module_tests
xmake run rtc_vehicle_control_module_tests
```
