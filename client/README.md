# 客户端目录

客户端代码分为可执行程序、共享协议和可复用模块，xmake 目标名称不受目录分层影响。

## apps

- `rtc_console/`：支持桌面交互和无渲染运行的 RTC 操作端。
- `rtc_edge_headless/`：工控机边缘端主程序，当前加载车辆控制配置。

应用目录只负责参数解析、依赖组装和生命周期管理。需要被多个应用复用的实现必须放入 `modules/`。

## 构建和运行

以下命令均在仓库根目录执行。运行前应先在 `config/rtc.cfg` 中配置可用的信令服务地址。

### rtc_console

构建并启动桌面界面：

```sh
xmake b rtc_console
xmake r rtc_console
```

`rtc_console` 登录 RTC 服务后默认自动打开 `zhejianglab` 房间，使用 `--room` 可指定其他房间：

```sh
xmake r rtc_console --room my-room
```

服务器无屏幕环境使用 `--no-render`，此模式不会创建 SDL 窗口或 OpenGL 上下文；`--headless` 是等价别名：

```sh
xmake r rtc_console --no-render --room my-room
```

也可以直接运行构建产物：

```sh
./build/runtime/rtc_console --no-render --room my-room
```

### rtc_edge_headless

`rtc_edge_headless` 仅在 Linux/Jetson 构建，需要摄像头、CUDA 和对应的 NVIDIA SDK 环境。使用构建时复制到运行目录的默认 `rtc.cfg` 启动：

```sh
xmake b rtc_edge_headless
xmake r rtc_edge_headless
```

使用 `--config` 指定其他配置文件；RTC、摄像头、视觉和车辆控制运行参数均从该文件的 `edge` 配置段读取：

```sh
xmake r rtc_edge_headless --config /path/to/rtc.cfg
```

也可以直接运行构建产物：

```sh
./build/runtime/rtc_edge_headless --config ./build/runtime/rtc.cfg
```

## tests

- `rtc_dual_camera_headless/`：双摄像头、RTC 和视觉链路的集成测试程序。
- `rtc_receiver_headless/`：RTC 视频接收和帧落盘测试程序。

测试程序保留原有 xmake 目标名，但不属于正式发布应用。

## protocols

- `vehicle/`：车辆控制、档位事务和状态反馈协议。
- `vision/`：视觉能力、类别表和检测帧协议。

协议 schema、nanopb 生成代码和 C++ codec 由客户端应用共享，不归属于某个具体可执行程序。详细约束与生成方式见 `protocols/README.md`。

## modules

- `camera/`：单相机 V4L2 采集、异步帧源和 I420 转换。
- `logging/`：基于 spdlog 的公共日志模块。
- `headless/`：RTC 会话和通用无界面运行参数。
- `edge/`：边缘端的单相机、双目和环视推流编排。
- `dual_camera/`：双摄像头异步图像源、帧订阅和 I420 转换。
- `vision/`：检测发送、双目融合、YOLO 消费与推理适配。
- `vehicle/`：车辆控制接收、安全门控和本地车辆控制适配。

模块公共头文件统一放在 `include/<命名空间>/`。跨模块包含必须使用完整前缀，例如：

```cpp
#include "rtc_headless/rtc_headless_session.h"
#include "rtc_dual_camera/dual_camera_async_image_source.h"
#include "rtc_edge/dual_camera_streaming_module.h"
#include "rtc_vision/vision_detection_sender.h"
#include "rtc_vehicle/vehicle_control_module.h"
```

`src/internal/` 仅供所属模块内部使用，不应加入其他模块的公共 include 路径。
