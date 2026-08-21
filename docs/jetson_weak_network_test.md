# Jetson Orin 弱网与动态分辨率测试指南

> 实现日期：2026-08-21<br>
> 测试平台：Jetson Orin，L4T R36.4.7，内核 `5.15.148-tegra`

## 1. 已完成内容

仓库新增了 `tools/rtc_weak_network.sh`，用于只针对指定 RTC 对端 IPv4 的 UDP 流量注入弱网条件。工具不会限制 SSH 和 WebSocket 信令使用的 TCP 流量，并提供状态记录、重复应用前清理、执行失败回滚和定时自动恢复。

工具支持两种后端：

- `tc`：系统内核具备 `PRIO`、`NETEM` 和 `IFB` 时，支持上下行限速、RTT、抖动、随机丢包和相关丢包。
- `iptables`：使用 `hashlimit` 对超过目标字节速率的 UDP 包执行 policing，并使用 `statistic` 注入随机丢包。该后端不能增加 RTT 和抖动，但足以触发 WebRTC 拥塞控制降低目标码率。

当前 Orin 内核配置为：

```text
CONFIG_NET_SCH_PRIO 未启用
CONFIG_NET_SCH_NETEM 未启用
CONFIG_IFB 未启用
CONFIG_NETFILTER_XT_MATCH_HASHLIMIT=m
CONFIG_NETFILTER_XT_MATCH_STATISTIC=m
```

因此当前设备会自动使用 `iptables` 后端，无需重新编译内核即可进行带宽和丢包联调。如果后续需要精确模拟 RTT、抖动和突发相关丢包，需要换用开启上述 qdisc/IFB 配置的内核。

## 2. 编码自适应配置

Jetson H.264 编码器已经开启 WebRTC QualityScaler：

- QP 降档阈值：36；
- QP 升档阈值：32；
- 最低像素数：`320×180`；
- 硬件 QP 可用范围：默认配置调整为 `[20,46]`；
- 全局最小目标码率：默认配置从 1.2 Mbps 调整为 150 kbps；
- 硬件编码器不再把 WebRTC 下发码率强制抬高到全局最小码率。
- 编码器每秒输出一次 QP 窗口统计，包含平均值、最小值、最大值、
  样本数、当前分辨率、码率和帧率，用于真实道路画面标定。
- `Release()` 后保留硬件会话 500 ms，WebRTC 紧接着调用 `InitEncode()`
  时直接复用；首帧前的连续尺寸请求会合并到真实输入帧上执行，避免
  Jetson 在空会话上连续 DRC。
- I420 写入 Jetson 对齐 Surface 前会校验可见尺寸并填充对齐区域，避免
  `720×450 -> 720×464`、`360×198 -> 368×208` 时越界读取。
- 每个原始输入帧使用唯一 V4L2 时间戳关联 `CaptureTask`。DRC 后同一输入
  产生 SPS 块和 IDR 块时不会再按 FIFO 误消费下一帧任务或提前回收其
  output buffer。
- `Release()` 会先注销回调并等待已进入的 `OnEncodedImage()` 返回；硬件
  会话仍按 500 ms 宽限期复用，但不会在上层销毁 callback 后访问旧指针。
- 接收端播放延迟上限从 10 ms 放宽到 400 ms，用少量缓冲吸收弱网抖动。
  这会改善画面连续性，但最差情况下也可能增加接近 400 ms 的观看延迟，
  不能把视频显示当作车辆闭环控制的唯一时序依据。

默认 30 fps 码率阶梯如下：

| 分辨率 | 启动码率 | 最低码率 | 最大码率 |
| --- | ---: | ---: | ---: |
| 320×180 | 175 kbps | 100 kbps | 450 kbps |
| 320×200 | 175 kbps | 100 kbps | 450 kbps |
| 480×270 | 400 kbps | 150 kbps | 800 kbps |
| 480×300 | 350 kbps | 150 kbps | 1.2 Mbps |
| 640×360 | 700 kbps | 150 kbps | 1.8 Mbps |
| 720×450 | 1.0 Mbps | 150 kbps | 2.2 Mbps |
| 960×540 | 1.6 Mbps | 150 kbps | 2.5 Mbps |
| 960×600 | 1.8 Mbps | 150 kbps | 2.8 Mbps |
| 1280×720 | 1.6 Mbps | 150 kbps | 6.8 Mbps |
| 1280×800 | 2.4 Mbps | 150 kbps | 6.8 Mbps |
| 1440×900 | 4.2 Mbps | 150 kbps | 7.8 Mbps |
| 1920×1080 | 6.2 Mbps | 150 kbps | 12 Mbps |
| 1920×1200 | 7.0 Mbps | 150 kbps | 12 Mbps |

当前 WebRTC 的升档约束不是只比较相邻表项，而是用
`floor(current_pixels × 5 / 3)` 再选择首个覆盖该像素数的码率档。因此各档
最大码率已保证能够越过实际选中的下一档启动门槛，并由集成测试覆盖表项
边界和非标准 16:9/16:10 输入，避免好网恢复后永久停在某个中间分辨率。

配置中的 `strategy` 仍可覆盖默认阶梯。自定义值的顺序为：`[min_start_bitrate_bps, min_bitrate_bps, max_bitrate_bps]`。

自动变化路径为：

```text
弱网限速/丢包
  -> WebRTC 传输反馈和拥塞控制降低目标码率
  -> JetsonH264EncoderImpl::SetRates()
  -> 当前分辨率的编码 QP 上升
  -> QualityScaler 修改 VideoSinkWants
  -> RtcVideoSource::VideoAdapter 缩放输入帧
  -> Jetson 编码器执行输出面 DRC 或热备切换
```

`rtc_edge` 的视频轨道使用 `kFluid`，因此 WebRTC 会优先维持运动帧率，再降低空间分辨率。

## 3. 使用方法

首先检查当前系统会使用哪个后端：

```sh
tools/rtc_weak_network.sh diagnose
```

`--target` 应填写实际承载媒体 UDP 的远端地址。不要只根据业务配置猜测，
应先从运行日志读取实际选中的 ICE 路径。日志目录相对于启动 `rtc_edge`
时的工作目录：从仓库根启动时是 `logs/rtc_agent.log`，从
`build/runtime` 启动时通常是 `build/runtime/logs/rtc_agent.log`：

```sh
tail -F logs/rtc_agent.log | \
  rg --line-buffered '\[NetPath\]'
```

当前 Orin 实测为 `local=10.15.73.29/.../udp(host)`、
`remote=10.15.73.245/.../udp(host)`、`relay=none`，因此直连测试使用
`--interface eth2 --target 10.15.73.245`。远端 UDP 端口会在重连后变化；
需要传 `--ports` 时必须重新读取本次会话的 `[NetPath]`。

只有 `host/udp` 直连路径才能直接把 `[NetPath] remote` 用作脚本目标。
若本地候选是 `relay`，需要结合 TURN 配置、`ss -unp` 或抓包确认 Orin
实际连接的 TURN 服务器地址；TURN over TCP/TLS 不会被本工具的 UDP 规则
命中。此时不得根据候选地址盲目下发规则。

> 实车安全要求：不带 `--ports` 时，脚本按目标 IP 聚合该机的全部 UDP，
> 可能同时影响视频、音频、RTCP、DataChannel、车辆控制和状态反馈。
> 弱网测试必须在车辆静止、驱动输出禁用且本地急停可用的条件下执行，
> 不得依赖 RTC 控制链路承担制动或失联保护。

应用弱网场景，默认 120 秒后自动恢复：

```sh
sudo tools/rtc_weak_network.sh profile weak \
  --interface eth2 \
  --target 10.15.73.245 \
  --duration 120
```

`profile`/`apply` 的自动恢复时间使用 `--duration`；`cycle` 每档持续时间
使用 `--step-duration`。脚本会拒绝混用，避免看似设置 45 秒却仍按默认
120 秒恢复。

验证发送端动态降分辨率时，建议先只限制 Orin 上行，并把随机丢包降到
1%。这样视频 RTP 会触发拥塞控制，远端返回的 RTCP、NACK 和 PLI 不会
同时受损，便于区分“码率适配失败”和“反馈链路被破坏”：

```sh
sudo tools/rtc_weak_network.sh apply \
  --interface eth2 \
  --target 10.15.73.245 \
  --rate 1200kbit \
  --loss 1 \
  --egress-only \
  --duration 45
```

确认码率和分辨率能够正常下降、画面能够恢复后，再使用双向 5% 丢包的
`profile weak` 做破坏性压力测试。该预定义场景会同时影响媒体流和同一
UDP 五元组上的反馈包，短暂冻结属于需要统计的压力测试结果。

执行完整的好网、逐级恶化和恢复序列：

```sh
sudo tools/rtc_weak_network.sh cycle \
  --interface eth2 \
  --target 10.15.73.245 \
  --egress-only \
  --step-duration 45
```

查看规则和实际丢包计数：

```sh
sudo tools/rtc_weak_network.sh status --interface eth2
```

立即恢复：

```sh
sudo tools/rtc_weak_network.sh clear --interface eth2
```

自定义 800 kbps、3% 丢包，只限制 Orin 上行并在 180 秒后恢复：

```sh
sudo tools/rtc_weak_network.sh apply \
  --interface eth2 \
  --target 10.15.73.245 \
  --rate 800kbit \
  --loss 3 \
  --egress-only \
  --duration 180
```

## 4. 预定义场景

| 场景 | 上下行带宽 | RTT | 抖动 | 每方向丢包 |
| --- | ---: | ---: | ---: | ---: |
| `good` | 12 Mbps | 20 ms | 2 ms | 0% |
| `limited` | 3 Mbps | 80 ms | 15 ms | 1% |
| `weak` | 1.2 Mbps | 160 ms | 40 ms | 5% |
| `extreme` | 400 kbps | 300 ms | 100 ms | 12% |
| `burst-loss` | 1 Mbps | 180 ms | 60 ms | 5%，60% 相关性 |

在当前 Orin 的 `iptables` 后端上，表中的带宽和随机丢包生效；RTT、抖动和丢包相关性会打印警告且不会静默假装生效。

`--rate` 是目标 IP 全部匹配 UDP 流的总预算，不是主画面独享带宽；共享
同一对端或 TURN 地址的五路视频、音频和 DataChannel 会共同竞争该预算。
`--egress-only` 时完成日志显示 `download=unchanged`，表示下行未受限制。

## 5. 观察与验收

建议每个场景至少持续 30～60 秒。自动降档通常比升档快，恢复到高分辨率需要等待 WebRTC 的质量稳定窗口，不能用一两秒的切换周期判断失败。

Jetson 默认码率阶梯中的 `min_start_bitrate_bps` 用于控制升档门槛；持续
运行最低码率必须允许降到全局弱网地板。若日志在 `weak` 阶段仍长期停在
`1920x1200 bitrate=4200000`，说明发送码率被高分辨率下限钳住，链路只
能在编码器之后丢包，对端通常会冻结而不会触发空间降档。

发送端日志重点关注：

```text
[JetsonEnc][rates] target_bitrate=... applied_bitrate=... fps=...
[JetsonEnc][qp] encoder=... size=... avg=... min=... max=... samples=... bitrate=... fps=... thresholds=32/36
[JetsonEnc][init] reused old=... target=... session=... ready=... elapsed_ms=...
[JetsonEnc][reconfigure] begin old=... new=...
[JetsonEnc][switch] Switched to prewarmed encoder ...
[NetPath] ... remote=... rtt_ms=... available_out_bps=...
[NetVideo] ... size=... fps=... actual_bps=... nack_delta=... pli_delta=...
```

当前 WebRTC 版本可能不提供 outbound RTP 的 `target_bitrate`，此时
`[NetVideo] target_bps=-1` 表示字段缺失，不表示目标为负数。编码目标以
`[JetsonEnc][rates] target_bitrate` 为准；`actual_bps` 是媒体
`bytes_sent` 统计，不包含 RTP/UDP/IP 头部，不能直接当作网卡线速。

Orin 上可以直接过滤实时日志：

```sh
tail -F logs/rtc_agent.log | \
  rg --line-buffered '\[JetsonEnc\]\[(rates|qp|init|session|switch|reconfigure)\]|\[Net(Path|Video)\]'
```

同时记录：

- `status` 中 `DROP` 包计数持续增长；
- WebRTC 目标码率和实际发送码率；
- 编码帧 QP；
- 编码输入宽高和接收端显示宽高；
- 帧率、冻结次数、最大连续无画面时间；
- RTT、NACK、PLI 和丢包统计。

建议的基本验收结果：

1. 从 `good` 降到 `weak/extreme` 后，目标码率先下降，随后分辨率逐档降低而不是积压发送队列。
2. 动态画面尽量保持接近 30 fps；到最低分辨率仍不足时才明显降低帧率。
3. 每次分辨率变化后的首帧可独立解码并包含 SPS+IDR。
4. 从弱网恢复后只逐档升分辨率，不发生每秒多次的分辨率乒乓。
5. `clear` 或定时回滚后，工具创建的链和状态文件全部消失。

## 6. 已执行验证

- `tools/tests/rtc_weak_network_tests.sh`：通过。
  - 校验脚本语法、参数拒绝和后端诊断；
  - 在独立用户/网络命名空间内实际创建上下行 `iptables` 链；
  - 发送约 2.4 MB UDP 数据，确认限速/丢包规则产生非零 DROP 计数；
  - 执行 `clear` 后确认链被完整删除。
- `rtc_jetson_h264_encoder_integration_tests 30`：180/180 次切换成功，全部首帧包含 SPS+IDR。
- `rtc_jetson_encoder_switch_benchmark 50`：300/300 次切换成功。
- `vtsrtc_aarch64`：Release 构建通过。
- `rtc_jetson_h264_encoder_integration_tests 3`：新增首帧前
  `1920×1200 -> 1440×900 -> 960×600 -> 720×450` 连续重配回归，输出首帧
  包含 SPS+IDR；后续 18 次上下行切换全部通过，平均回调间隙
  23.759 ms、p95 38.856 ms、最大 62.868 ms。测试会分别覆盖另一线程
  `Release()` 等待阻塞回调，以及旧回调内部执行
  `Release -> Register -> InitEncode`；新会话回调在旧回调退出前被串行屏障
  阻挡 50 ms。Release 前已排队的旧会话帧因 callback epoch 变化被丢弃，
  随后新生命周期恢复编码。
- 最终 DRC 状态机执行 `rtc_jetson_encoder_switch_benchmark 10`：60/60 次
  切换均包含 SPS+IDR，重配平均 3.042 ms、p95 5.553 ms、最大 7.890 ms；
  首个可交付回调间隙平均 18.622 ms、p95 30.989 ms、最大 32.739 ms。
- 真实 `rtc_edge` + Jetson NVENC + 双路 `1920×1200` 摄像头：最终版本持续
  运行约 98 秒，启动阶段完成
  `720×450 -> 960×600 -> 1440×900 -> 1920×1200`，随后真实带宽波动又
  完成 `1920×1200 -> 1440×900 -> 1920×1200`。主流全程保持 29～30 fps，
  76 个原生分辨率 QP 窗口均约有 30～31 个有效样本且 `missing=0`；退出前
  `captured=3227`、`sent=3202`，未再出现
  `Surface resolution smaller than encode resolution`、输入 Surface 尺寸不匹配、
  Plane 不匹配、捕获回调空缓冲、不可用会话或 `NvVideoEnc BlockError`。
  本次自然降档的 output-plane DRC 为 5.087 ms，约 10 秒后顺利恢复原生
  分辨率。
- 加入外部回调串行化和 callback epoch 后重新构建正式动态库并再次运行
  约 86 秒：退出前 `captured=2886`、`sent=2841`，主流保持
  `1920×1200@29～30fps`，69 个原生分辨率 QP 窗口全部 `missing=0`，
  上述活动会话错误和 callback 交付失败计数仍全部为零。
- 本次 `[NetPath]` 验证为 `eth2` 上直连 UDP，远端为
  `10.15.73.245`；该地址仅代表本次测试，后续仍应以实时日志为准。

当前自动化会话没有可用的 sudo 凭据，因此没有直接在主机 `eth2` 上执行
完整 `cycle`。工具本身的规则执行已经在隔离网络命名空间中完成真实内核
验证；主机联调可直接使用本次确认的目标执行上文命令，并按本节指标采集
完整数据。
