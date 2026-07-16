# camera 模块

`rtc_camera` 统一提供单摄和双摄的采集、帧订阅与格式转换能力，不依赖具体应用、视觉算法或车辆业务。

目录结构：

- `include/rtc_camera/` 和 `src/` 根目录：单摄与双摄共享的 V4L2 设备和采集参数。
- `single/`：单摄异步帧源以及 NV12、UYVY、YUYV 到 I420 的转换。
- `dual/`：双摄异步帧源、独立订阅队列和双路画面转换。
- `single/internal/` 和 `dual/internal/`：CUDA 转换及双摄采集内部实现，外部模块不得直接依赖。

公共命名空间：

```cpp
rtc_camera
rtc_camera::single
rtc_camera::dual
```

单摄调用方包含：

```cpp
#include "rtc_camera/single/async_image_source.h"
#include "rtc_camera/single/frame_converter.h"
```

双摄调用方包含：

```cpp
#include "rtc_camera/dual/async_image_source.h"
#include "rtc_camera/dual/frame_converter.h"
```

异步订阅队列满时会丢弃旧帧并保留最新帧，避免网络、编码或视觉处理变慢后继续消费过时的视频帧。双目视觉匹配和检测融合仍属于 `vision` 模块。
