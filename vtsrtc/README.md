## vtsrtc 目录说明

- `src/` 是 vtsrtc 通信库源码。
- `p2p_imgui/` 是基于 ImGui 的交互式示例。
- `rtc_headless_common/` 是 headless 发送端和接收端复用的公共代码，包括参数解析、日志、RTC 会话和 V4L2 采集工具。
- `rtc_camera_headless/` 是单摄 headless 发送端。
- `rtc_dual_camera_headless/` 是双摄 headless 发送端，对外提供可复用的异步图像接口。
- `rtc_receiver_headless/` 是 headless 接收端。
