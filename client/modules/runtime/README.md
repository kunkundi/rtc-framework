# runtime 模块

`runtime` 提供 Linux 客户端应用可复用的运行时基础设施，不包含摄像头、视觉或车辆业务逻辑。

- `process_runtime.h`：安装退出信号处理、维护停止状态，并解析可执行程序附近的配置文件路径。
- `rtc_session.h`：封装 RTC 初始化、自动进入房间、数据通道与视频源注册、状态统计和有序关闭。

应用负责解析自身参数并组装 `SessionOptions` 与 `RtcSession::Features`。摄像头采集参数应由 `camera` 或 `dual_camera` 模块定义，数据通道和视频源应由使用对应协议的应用注册，避免运行时模块依赖具体设备或业务类型。
