## vtsrtc 目录说明

- `src/` 是 RTC 通信库源码。
- `vtsrtc.map` 是 Linux 导出符号版本脚本。
- 对外稳定接口位于 `src/c_rtc.h`，客户端应用和模块统一位于仓库根目录的 `client/`。
