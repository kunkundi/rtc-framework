# rtc_edge

`rtc_edge` 是运行在工控机上的边缘端主程序，统一编排 RTC、摄像头、音频采集播放和设备控制配置，仅在 Linux/Jetson 构建。

## 模块结构

- `client/apps/rtc_edge/main.cpp`：进程入口，安装信号处理并报告未捕获异常。
- `client/apps/rtc_edge/edge_options.*`：定义应用选项，解析并校验命令行参数。
- `client/apps/rtc_edge/edge_application.*`：创建各子模块，管理 RTC 回调、主循环和关闭顺序。
- `client/apps/rtc_edge/edge_options_tests.cpp`：覆盖配置读取、必填项和车辆看门狗上限。
- `client/modules/edge/`：与设备类型无关的摄像头推流编排。
- `client/modules/vehicle/`：车辆控制接收、安全门控和本地控制接口。
- `client/modules/camera/`：公共 V4L2 能力以及单摄、双摄采集和转换。
- `client/modules/audio/`：跨平台音频设备枚举、PCM 采集和播放。
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

音频数据流：

```text
输入设备 -> rtc_audio 10 ms PCM 帧 -> edge_audio RTC 音频轨道
远端 RTC 音频帧 -> rtc_audio 播放流 -> 输出设备
```

## 相机视频通道

RTC 会话预先协商六个外部视频源，实际仅发送当前视图需要的画面：

| 视频源 ID | 内容 | 当前状态 |
| --- | --- | --- |
| `merged_image` | 双目拼接画面 | 已接入采集和发送 |
| `surround_grid` | 四路环视田字形拼接 | 每格 320×180，总计 640×360 |
| `surround_front` | 环视前摄像头 | 已接入采集和发送 |
| `surround_rear` | 环视后摄像头 | 已接入采集和发送 |
| `surround_left` | 环视左摄像头 | 已接入采集和发送 |
| `surround_right` | 环视右摄像头 | 已接入采集和发送 |

双目始终在编码前左右拼接，作为一路视频发送。默认仪表盘模式发送双目和 `surround_grid`，四路环视在编码前使用 libyuv 缩放后拼成田字格；未配置的摄像头位置保持黑色。环视模式仅发送 `surround_grid`。点击接收端的环视象限后，仅发送对应的 1280×720 单摄画面，双目及其他环视源均停止提交帧，不发送后台保活帧。再次点击单摄画面或按 Esc 恢复默认仪表盘模式。

接收端按发送端会话排列，默认两列且至少两行，每个发送端的环视默认位于该发送端主画面的右下角。视图切换通过可靠有序的 `video.view_control.v1` 定向发送，不会自动切换车辆控制目标。协议 minor 为 1，模式 1 为仪表盘、2 为环视、3/4/5/6 分别为前/后/左/右单摄；两端需配套更新。RTC 入队成功不等于远端完成切换，界面等待新的解码帧，网络自适应仍可能降低实际解码分辨率。

默认设备映射为双目 `/dev/video0`、`/dev/video1`，环视 `/dev/video2` 至 `/dev/video5`。请按实际安装方向调整设备映射；本实现是画面排列拼接，不包含几何标定、畸变校正或鸟瞰融合。

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
    "audio": {
      "input_enabled": true,
      "output_enabled": true,
      "input_device": "default",
      "output_device": "default",
      "sample_rate": 48000,
      "channels": 1
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

`audio` 配置段可省略，省略时音频输入输出均关闭。启用后，`input_device` 和 `output_device` 使用 SDL 报告的完整设备名称，填写 `default` 表示跟随系统默认设备；设备名称不匹配时，启动错误会列出当前可用设备。输入支持 8000、16000、32000、44100 或 48000 Hz，以及单声道或双声道，发送端固定按 10 ms、16-bit PCM 帧传输。示例配置可先保持输入输出关闭，再根据实际设备逐项启用。

`stereo_camera` 配置双目左右设备，`surround_camera` 配置环视前后左右四个设备，各组相机在组内共享采集参数。环视设备路径为空时不启用对应视频源。

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
