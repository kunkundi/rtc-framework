# rtc_edge

`rtc_edge` 是运行在工控机上的边缘端主程序，统一编排 RTC、双目摄像头采集和设备控制配置。当前只实现车辆控制配置，且仅在 Linux/Jetson 构建。

## 模块结构

- `client/apps/rtc_edge/main.cpp`：进程入口，安装信号处理并报告未捕获异常。
- `client/apps/rtc_edge/edge_options.*`：定义应用选项，解析并校验命令行参数。
- `client/apps/rtc_edge/edge_application.*`：创建各子模块，管理 RTC 回调、主循环和关闭顺序。
- `client/apps/rtc_edge/edge_options_tests.cpp`：覆盖配置读取、必填项和车辆看门狗上限。
- `client/modules/edge/`：与设备类型无关的摄像头推流编排。
- `client/modules/vehicle/`：车辆控制接收、安全门控和本地控制接口。
- `client/modules/dual_camera/`：双摄像头异步采集和 I420 转换。
- `client/modules/runtime/`：进程生命周期、配置路径解析和 RTC 会话。

控制数据流：

```text
RTC DataChannel
    -> VehicleControlModule 消息队列
    -> 协议解码、序号与看门狗校验
    -> VehicleControlInterface
    -> 工控机/CAN/串口/TCP/厂商 SDK
```

摄像头数据流：

```text
双目摄像头
    -> AsyncDualCameraImageSource
    -> DualCameraStreamingModule
    -> I420 拼接帧
    -> RtcSession
```

## 相机视频通道

RTC 会话使用五个独立的外部视频源：

| 视频源 ID | 内容 | 当前状态 |
| --- | --- | --- |
| `merged_image` | 双目拼接画面 | 已接入采集和发送 |
| `surround_front` | 环视前摄像头 | 通道已注册，待接入采集 |
| `surround_rear` | 环视后摄像头 | 通道已注册，待接入采集 |
| `surround_left` | 环视左摄像头 | 通道已注册，待接入采集 |
| `surround_right` | 环视右摄像头 | 通道已注册，待接入采集 |

双目仍作为一路完整视频发送，不拆分左右目通道。环视四路不做拼接，后续采集模块应使用对应的视频源 ID 调用 `SendI420Frame`。

## 工控机接口

当前主程序使用 `PlaceholderVehicleControlInterface`。该实现允许摄像头和 RTC 正常运行，但会拒绝所有驾驶及换档指令，并明确返回“车辆控制接口尚未接入”，不会假装设备已经执行。

接入真实工控机时，实现 `VehicleControlInterface`：

```cpp
class VehicleControlInterface {
 public:
  virtual bool Open(std::string* error_message) = 0;
  virtual void Close() = 0;
  virtual VehicleCommandResult SendDriveCommand(
      const vts_rtc::vehicle::DriveCommand& command) = 0;
  virtual VehicleCommandResult SendGearCommand(
      vts_rtc::vehicle::VehicleGear gear) = 0;
  virtual void SendStop() = 0;
};
```

然后在 `edge_application.cpp` 中将占位实现替换为具体实现。所有方法都在边缘端主线程调用，可以在实现中接入 CAN、串口、TCP 或厂商 SDK。`SendStop()` 必须实现为本地安全停车操作。

## 构建和运行

Linux/Jetson 需要现有双摄像头依赖和 CUDA 运行环境：

```sh
xmake b rtc_edge
xmake r rtc_edge --config rtc.cfg
```

源码树中的配置文件位于 `config/rtc.cfg`，构建时会复制到可执行程序目录，
因此上述命令仍可直接使用 `rtc.cfg`。除了用于选择配置文件的 `--config` 和显示帮助的
`--help`，其他运行参数都从该配置读取。网络、编码器和分辨率限制继续使用原有根配置，
边缘端专用参数位于 `edge` 配置段：

```json
{
  "edge": {
    "rtc": {
      "room": "zhejianglab",
      "join_retry_ms": 3000,
      "status_interval_sec": 5,
      "frame_limit": 0
    },
    "stereo_camera": {
      "left_device": "/dev/video0",
      "right_device": "/dev/video1",
      "width": 1280,
      "height": 720,
      "buffer_count": 4,
      "timeout_ms": 2000,
      "warmup_frames": 0,
      "warmup_delay_ms": 0,
      "frame_wait_ms": 20
    },
    "surround_camera": {
      "front_device": "/dev/video2",
      "rear_device": "/dev/video3",
      "left_device": "/dev/video4",
      "right_device": "/dev/video5",
      "width": 1280,
      "height": 720,
      "buffer_count": 4,
      "timeout_ms": 2000,
      "warmup_frames": 0,
      "warmup_delay_ms": 0,
      "frame_wait_ms": 20
    },
    "vehicle_control": {
      "watchdog_ms": 300,
      "state_interval_ms": 50,
      "max_pending_events": 128
    }
  }
}
```

`edge` 中列出的字段都是必填项。`stereo_camera` 配置双目左右设备，`surround_camera` 配置环视前后左右四个设备，各组相机在组内共享采集参数。当前环视配置会被读取和校验，实际采集模块仍待接入。

`watchdog_ms` 不能超过协议规定的 300 ms 上限。视频源 ID、DataChannel label 和安全上限属于稳定协议约束，不作为可配置项。
为避免部分系统终端编码不兼容，配置文件和命令行错误信息统一使用 ASCII 英文。

协议错误、看门狗超时、控制队列溢出、RTC 链路断开或本地车辆控制接口拒绝指令后，控制模块会锁存安全停车。本次 P2P 连接中的后续驾驶和换档命令不会解除锁停，必须重新建立控制连接。
单条控制消息的接收上限为 4 KiB，空载荷、无效指针和超限载荷均按协议错误处理。

控制模块单测不依赖摄像头或工控机，可在 Windows/Linux 单独运行：

```sh
xmake b rtc_vehicle_control_module_tests
xmake r rtc_vehicle_control_module_tests
xmake b rtc_edge_options_tests
xmake r rtc_edge_options_tests
```
