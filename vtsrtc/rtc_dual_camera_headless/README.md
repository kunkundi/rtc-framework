## rtc_dual_camera_headless

本目录保存双目 headless 客户端的专属代码。

目录分层：

- `include/rtc_dual_camera/` 是对外公开 API。调用方只需要依赖 `rtc_dual_camera_image_source` 目标并包含这里的头文件。
- `src/` 是公开 API 的实现。
- `src/internal/` 是内部采集实现，不建议外部直接依赖。
- `consumers/` 是独立消费者实现。后续接 YOLO 推理时可以从 `consumers/yolo_frame_consumer.cpp` 里的 `ProcessYoloFrame` 开始补充。
- `app/` 是 `rtc_dual_camera_headless` 可执行程序入口，只负责命令行解析、RTC 会话和消费者编排。

调用示例：

```cpp
#include "rtc_dual_camera/dual_camera_async_image_source.h"

#include <chrono>

rtc_dual_camera::AsyncDualCameraImageSourceOptions options;
options.left_device = "/dev/video0";
options.right_device = "/dev/video1";
options.width = 1280;
options.height = 720;

rtc_dual_camera::AsyncDualCameraImageSource source(options);
auto frames = source.Subscribe(2);
source.Start();

rtc_dual_camera::ImageFrame frame;
if (frames->WaitNext(&frame, std::chrono::milliseconds(100)) &&
    frame.format == rtc_dual_camera::ImagePixelFormat::kDualUyvy &&
    !frame.empty()) {
  // frame.left_data / frame.right_data 是左右摄像头原始 UYVY 图像。
  // 消费端按自己的需要转换成 I420、RGB tensor 或其他格式。
}
```

多个消费者调用示例：

```cpp
#include "rtc_dual_camera/dual_camera_async_image_source.h"

#include <atomic>
#include <chrono>
#include <thread>

std::atomic<bool> stop_requested(false);

rtc_dual_camera::AsyncDualCameraImageSourceOptions options;
options.left_device = "/dev/video0";
options.right_device = "/dev/video1";
options.width = 1280;
options.height = 720;

rtc_dual_camera::AsyncDualCameraImageSource source(options);

// 每个消费者使用独立的订阅队列。队列满时会丢弃旧帧，保留最新帧。
auto rtc_frames = source.Subscribe(2);
auto yolo_frames = source.Subscribe(1);

source.Start();

std::thread rtc_consumer([&] {
  while (!stop_requested.load()) {
    rtc_dual_camera::ImageFrame frame;
    if (!rtc_frames->WaitNext(&frame, std::chrono::milliseconds(50))) {
      continue;
    }
    if (frame.format != rtc_dual_camera::ImagePixelFormat::kDualUyvy ||
        frame.empty()) {
      continue;
    }

    // 将原始双路 UYVY 转成 RTC 需要的格式后发送。
  }
});

std::thread yolo_consumer([&] {
  while (!stop_requested.load()) {
    rtc_dual_camera::ImageFrame frame;
    if (!yolo_frames->WaitNext(&frame, std::chrono::seconds(1))) {
      continue;
    }
    if (frame.empty()) {
      continue;
    }

    // 将当前最新原始帧保存、分析或转交给其他模块。
  }
});

// 退出时先通知线程，再关闭订阅并停止采集。
stop_requested.store(true);
rtc_frames->Close();
yolo_frames->Close();
rtc_consumer.join();
yolo_consumer.join();
source.Stop();
```
