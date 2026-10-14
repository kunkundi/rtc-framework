## vtsrtc 目录说明

- `src/` 是 RTC 通信库源码。
- `vtsrtc.map` 是 Linux 导出符号版本脚本。
- 对外稳定接口位于 `src/c_rtc.h`，客户端应用和模块统一位于仓库根目录的 `client/`。

## 视频延迟统计

`RtcSetVideoDelayStatsHandlerV1` 是兼容性新增接口，不改变原有 `RtcNetStats`、初始化参数或回调签名。初始化成功后注册，销毁 Agent 时自动清空。回调与网络统计回调在同一线程同步调用，载荷和 source ID 仅在回调期间有效；注销不等待已进入的回调完成。当前需要同时注册原网络统计回调并启用配置中的 `netstats_report`。

`RtcVideoDelayStatsV1` 使用毫秒，负数表示不可用，零是有效值。首个样本仅建立累计计数基线；缓冲和解码采用后续报告之间的增量均值，窗口无新帧或计数重置时返回不可用。状态按 session 和 SSRC 隔离，断开或重置时清理。

- `network_ms`：选中 ICE candidate pair 的 RTT/2，仅为单向网络近似。
- `jitter_buffer_ms`：接收 jitter buffer 累计停留时间除以释放帧数的窗口增量。
- `decode_ms`：累计解码时间除以解码帧数的窗口增量。
- `estimated_receive_ms`：上述三项齐备时的合计估算。不是逐帧实测，不含采集、发送端处理与编码、发送排队、应用渲染和屏幕刷新；不同统计的边界和窗口也可能不同。

旧 `delay_ms` 返回合计估算的整数毫秒，不可用时仍为 0；需要区分缺失与真实零值的客户端应使用新接口。旧二进制客户端可以继续使用新库，使用新接口的客户端需要更新动态库。

验证：`xmake b rtc_video_delay_tests && xmake r rtc_video_delay_tests`，以及 `xmake b rtc_video_delay_abi_tests && xmake r rtc_video_delay_abi_tests`。
