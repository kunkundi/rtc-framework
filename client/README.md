# 客户端目录

客户端代码分为可执行程序和可复用模块，xmake 目标名称不受目录分层影响。

## apps

- `p2p_imgui/`：桌面交互式 RTC 客户端。
- `rtc_camera_headless/`：单摄像头无界面发送端及其 CUDA 转换代码。
- `rtc_dual_camera_headless/`：双摄像头、RTC 和视觉消费者的编排入口。
- `rtc_receiver_headless/`：无界面接收端。
- `rtc_vehicle_headless/`：车辆端主程序和运行说明。

应用目录只负责参数解析、依赖组装和生命周期管理。需要被多个应用复用的实现必须放入 `modules/`。

## modules

- `headless/`：RTC 会话、通用参数、日志和 Linux V4L2 采集支持。
- `dual_camera/`：双摄像头异步图像源、帧订阅和 I420 转换。
- `vision/`：检测发送、双目融合、YOLO 消费与推理适配。
- `vehicle/`：车辆摄像头模块、控制接收、安全门控和工控机抽象。

模块公共头文件统一放在 `include/<命名空间>/`。跨模块包含必须使用完整前缀，例如：

```cpp
#include "rtc_headless/rtc_headless_session.h"
#include "rtc_dual_camera/dual_camera_async_image_source.h"
#include "rtc_vision/vision_detection_sender.h"
#include "rtc_vehicle/vehicle_control_module.h"
```

`src/internal/` 仅供所属模块内部使用，不应加入其他模块的公共 include 路径。
