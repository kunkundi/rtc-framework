# 车辆控制协议 v1

`rtc_vehicle_control_protocol` 是面向工程车辆远程驾驶的精简 nanopb 协议，当前仅支持：

- 前进、后退和停止
- 方向盘左转、右转和回中
- 归一化油门和刹车
- 前进档、倒档和空档

该协议以原生 C++ 库形式提供。它不负责对端身份认证、直接控制电机，也不能替代设备本地的硬件安全联锁。

## DataChannel

设备端作为加入房间的一方，在加入房间前创建以下通道：

| Label | 优先级 | 传输方式 | 载荷 |
| --- | --- | --- | --- |
| `vehicle.control.v1` | High | 乱序，`max_retransmits=0` | 驾驶中的 `DriveCommand`，50 Hz |
| `vehicle.event.v1` | High | 有序、可靠 | `SetGear` 和 `EventAck` |
| `vehicle.state.v1` | Medium | 乱序，`max_retransmits=0` | `VehicleState`，20 Hz 并在档位结果产生时立即发送 |

三类消息都应通过 `RtcSendData(remote_sessionid, label, payload.data(), payload.size())` 发送。禁止使用 `RtcBroadcastData` 广播车辆控制指令。

## 线协议消息

每个载荷都是 `VehicleControlEnvelope`，包含魔数 `VCL1`、协议版本 `1.0`、消息类型和发送端单调递增序号。

`DriveCommand` 是实时驾驶帧：

- `drive_direction` 为停止、前进或后退。
- `steering_direction` 为回中、左转或右转。
- `throttle` 和 `brake` 是 `[0, 1]` 范围内的有限 `float32` 值。

协议允许油门和刹车同时非零，具体仲裁规则由本地车辆控制器决定。

`SetGear` 是可靠事务，包含非零 `request_id`，档位为前进档、倒档或空档。设备通过 `EventAck` 返回相同的请求 ID、是否接受、错误码和当前有效档位。

`VehicleState` 返回当前有效档位、最近接受的驾驶帧序号，以及协议看门狗是否已经触发停车。

## 安全规则

- 驾驶过程中每 20 ms 发送一个新的驾驶帧。
- `DriveCommandGate` 丢弃重复或乱序的驾驶序号，并且这些无效帧不会刷新看门狗。
- 默认看门狗时间为 300 ms。看门狗超时、油门或刹车越界、出现 NaN/Inf 时，门控器要求本地安全停车。
- 设备控制器必须将门控器的 `should_stop` 结果转换为实际制动或电机禁用操作。
- 物理急停、限位和本地安全逻辑始终拥有最高优先级。
- 空档是可靠的档位状态变化，不能根据实时驾驶帧缺失自动推断为空档。

## 公共 C++ API

包含头文件 `rtc_control/vehicle_control_protocol.h`。编解码接口包括：

- `EncodeDriveCommand`
- `EncodeSetGear`
- `EncodeEventAck`
- `EncodeVehicleState`
- `DecodeEnvelope`

v1 的所有二进制载荷均不超过 50 字节。

`DriveCommandGate` 是接收端使用的序号和 300 ms 看门狗辅助类。在本地控制器准备完成后调用 `Start(now_ms)`；每次解码出驾驶帧后调用 `Accept(seq, command, now_ms)`；在设备控制循环中持续调用 `PollWatchdog(now_ms)`。

## 生成、构建与测试

修改 schema 后重新生成 nanopb 源文件：

```powershell
python tools/generate_nanopb.py --protocol control
```

构建并运行协议测试：

```powershell
xmake build rtc_vehicle_control_protocol_tests
xmake run rtc_vehicle_control_protocol_tests
```
