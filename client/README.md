# 客户端目录

客户端代码分为可执行程序和可复用模块，xmake 目标名称不受目录分层影响。

## apps

- `p2p_imgui/`：桌面交互式 RTC 客户端。
- `rtc_edge_headless/`：工控机边缘端主程序，当前加载车辆控制配置。

应用目录只负责参数解析、依赖组装和生命周期管理。需要被多个应用复用的实现必须放入 `modules/`。

## tests

- `rtc_dual_camera_headless/`：双摄像头、RTC 和视觉链路的集成测试程序。
- `rtc_receiver_headless/`：RTC 视频接收和帧落盘测试程序。

测试程序保留原有 xmake 目标名，但不属于正式发布应用。

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
