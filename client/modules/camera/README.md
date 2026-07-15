# camera 模块

`rtc_camera` 提供单个物理相机的通用能力，不依赖 headless 应用或 edge 业务。

主要接口：

- `CameraCaptureOptions`：相机设备、分辨率、缓冲区和预热参数。
- `V4l2CameraDevice`：Linux V4L2 设备的打开、取帧和缓冲区归还。
- `AsyncCameraImageSource`：在独立线程采集相机帧，并向消费者提供有界订阅队列。
- `CameraFrameConverter`：将 NV12、UYVY 或 YUYV 转换成 RTC 使用的 I420。

订阅队列满时会丢弃旧帧并保留最新帧，避免网络或编码暂时变慢后继续发送已经过时的视频。

edge 环视功能通过四个 `SingleCameraStreamingModule` 实例复用本模块；双目模块则组合两个 `V4l2CameraDevice` 完成左右帧采集。
