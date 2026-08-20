# 音频设备模块

`rtc_audio` 使用 SDL3 统一封装 Windows、Linux 和 Jetson 上的音频设备枚举、PCM 采集和播放。

- 输入固定输出 16-bit interleaved PCM，并按配置的 10 ms 帧长回调给 RTC 发送端。
- 输出按远端 PCM 帧格式创建 SDL 音频流，由 SDL 完成到实际播放设备格式的转换。
- 播放队列超过约 500 ms 时会清空旧数据，避免网络抖动后持续播放过期音频。
- 设备 ID 仅用于一次枚举结果；持久化和重新打开设备时使用设备名称，`default` 表示系统默认设备。

音频采集和播放由 `rtc_edge` 与 `rtc_console` 复用。模块不修改 `vtsrtc` 的导出 C ABI。
