# Jetson H.264 编码低延时与极端网络自适应调研报告

> 调研日期：2026-08-20<br>
> 仓库基线：`standalone` 分支，提交 `6c2d6e3b098d`

## 1. 结论摘要

当前 Jetson 编码器在分辨率变化时发生严重卡顿的直接原因，不是 C++ 对象析构本身，而是 `JetsonEncoder::Reconfigure()` 实际执行了近似完整的 V4L2 编码流水线重启：停止 DQ 线程、同时 `STREAMOFF` 输出面和捕获面、释放两侧缓冲区、重新设置格式并启动线程。代码中还存在最长约 1～2 秒的线程等待/中止路径，因此动态带宽触发分辨率切换时很容易产生明显黑屏或冻结。

推荐方案按优先级排列如下：

1. **主方案：保持单个 NVENC 会话存活，按 NVIDIA 官方样例只重配 V4L2 输出面。** 编码会话以码率阶梯的最高分辨率初始化捕获面；切换输入分辨率时，仅排空、停止、释放和重建原始帧输出面，捕获面及其 DQ 线程保持运行。切换后立即请求 IDR，并携带 SPS/PPS。普通阶梯切换不得销毁/创建编码器。
2. **码率和帧率原地更新。** `NvVideoEncoder::setBitrate()` 与 `setFrameRate()` 官方均允许在格式设置后调用。当前代码只动态更新码率，却在帧率变化时仅打印“需要重建”的警告，应改为原地更新。
3. **优先保帧率，再逐级降低空间分辨率。** 当前发送端已使用 `kFluid` 内容提示，WebRTC 会倾向 `MAINTAIN_FRAMERATE`，这符合动态画面的目标。仅当最低分辨率仍不可维持时，再把帧率从 30 逐步降到 24、20、15 fps；不要通过积压编码队列来“保清晰度”。
4. **重新启用 WebRTC 质量缩放并修正码率/QP配置。** 当前实现关闭了 `scaling_settings`，默认分辨率码率限制又近似为 `1 bps`，使 WebRTC 缺少正确的升降档依据。配置中的 `bitrate_minmum=1.2 Mbps` 还可能强迫编码器在网络目标更低时继续超发，形成 pacer 排队和不断增长的端到端延时。
5. **仅将热备编码器作为兼容性兜底。** 如果目标 JetPack/SoC 不支持所需的输出面 DRC，或切换目标超过会话初始上限，才使用有全局资源预算的异步热备编码器。热备必须实际编码并丢弃至少一帧完成预热，切换时等待新编码器的 IDR/SPS/PPS 已提交后再回收旧编码器。

不存在一种编码参数能在带宽低于画面信息量时，同时长期保持 1080p、30 fps、高画质和极低延时。极端网络下的正确优先级应是：**不积压、保连续运动、逐级降分辨率、最后降帧率；清晰度在当前可用码率内最大化。**

## 2. 调研范围

本报告检查了以下实现路径：

- `vtsrtc/src/video/encode/nvidia-jetson/jetsonh264_encoder_impl.cpp`
- `vtsrtc/src/video/encode/nvidia-jetson/jetson_encoder.cpp`
- `vtsrtc/src/video/encode/nvidia-jetson/NvVideoEncoder.cpp`
- `vtsrtc/src/rtc_connection_manager.cpp`
- 摄像头 I420 转换、`RtcSendFrame` 和 WebRTC `VideoEncoder` 接入路径
- 仓库中 2026 年的 Jetson 编码器池、异步预热和单编码器重配历史实现
- 本机 Jetson Multimedia API 头文件及 `01_video_encode` 官方样例
- NVIDIA Jetson Multimedia API、Accelerated GStreamer 和 WebRTC 编码器自适应相关文档

本报告最初结论基于代码静态检查和官方资料对照。后续已在 Jetson Orin、L4T R36.4.7 上完成真实 NVENC 测试和实现验证，详见[Jetson 分辨率切换硬件测试报告](jetson_encoder_switch_hardware_benchmark.md)。其他 JetPack、SoC 和驱动组合仍需分别验证。

## 3. 当前数据路径与问题定位

### 3.1 当前路径

```text
摄像头帧
  -> CUDA/CPU 转为 I420
  -> RtcSendFrame
  -> BuildAndLimitFrameSize 中复制为 WebRTC I420Buffer
  -> JetsonH264EncoderImpl::Encode
  -> 再复制到 V4L2 MMAP YUV420M 输出缓冲区
  -> Jetson NVENC
  -> 捕获面 DQ 线程取出 H.264
  -> WebRTC OnEncodedImage
```

这条路径至少存在两次完整图像复制：进入 WebRTC I420 缓冲区一次，I420 到 V4L2 MMAP 缓冲区一次。在 1080p/30 fps、多摄像头或 CPU/GPU 负载高时，复制时间和内存带宽会放大编码抖动，但它不是分辨率切换长卡顿的首要原因。

### 3.2 分辨率切换为什么卡顿

`JetsonH264EncoderImpl::EnsureEncoderForResolution()` 在宽高变化时调用 `JetsonEncoder::Reconfigure()`。后者当前会：

1. 设置停止状态并停止捕获面 DQ 处理；
2. 对输出面和捕获面同时执行 `STREAMOFF`；
3. 等待 DQ 线程退出，超时路径还会调用 abort；
4. 同时释放输出面和捕获面缓冲区；
5. 重新设置输出、捕获格式和全部编码参数；
6. 重新申请缓冲区、启动两侧 V4L2 流和 DQ 线程；
7. 请求关键帧。

因此，虽然正常路径复用了 `JetsonEncoder` C++ 对象，但硬件/V4L2 流水线仍被整体停止和重启，效果与重建编码器接近。线程等待常量为 1000 ms，异常路径可能进一步增加停顿。

NVIDIA 官方 `01_video_encode` 样例的动态分辨率切换实现不同：它仅排空并重建原始帧**输出面**，保持编码码流**捕获面**和 DQ 线程运行。样例说明主要覆盖从高分辨率向低分辨率切换；能否在不重建捕获面的情况下可靠地从低分辨率升回初始上限以内，需要在本项目目标平台验证，不能仅凭 API 推断。

### 3.3 当前自适应配置的关键矛盾

| 项目 | 当前状态 | 影响 |
| --- | --- | --- |
| WebRTC 质量缩放 | `GetEncoderInfo()` 返回 `ScalingSettings::kOff` | QP/丢帧无法驱动空间分辨率自适应 |
| 分辨率码率限制 | 默认每档 `min_start_bitrate_bps` 和 `min_bitrate_bps` 近似为 1 | 带宽分配器会认为任意分辨率在极低码率下仍可工作 |
| 编码码率下限 | `bitrate_minmum=1200000` | 当 WebRTC 目标低于 1.2 Mbps 时，编码器仍可能按 1.2 Mbps 输出，引起发送队列增长 |
| QP 范围/阈值 | QP `[30,36]`，阈值 `[34,38]` | 最大 QP 36 永远达不到降档阈值 38；好网络下最小 QP 30 又限制画质上限 |
| 动态帧率 | `SetRates()` 只更新码率 | WebRTC 目标帧率变化未实际传给硬件 |
| 低延时播放要求 | `playout_delay` 为 0～10 ms | 这是接收端抖动缓冲要求，不是编码时延；极端抖动下可能增加卡顿 |
| 编码队列满 | 非阻塞取回失败时直接丢帧 | 低延时方向正确，但未明确反馈 `OnDroppedFrame`，不利于质量缩放和统计 |
| 编码档次 | 固定 Baseline，但工厂还可能宣告 High | 协商能力与实际码流不完全一致；High/CABAC 的压缩收益目前没有真正启用 |

## 4. 推荐总体架构

### 4.1 单会话输出面 DRC

建议把当前 `Reconfigure()` 拆为两类操作：

- `ReconfigureOutputPlane(width, height)`：普通码率阶梯切换，只处理原始帧输出面；
- `RestartEncoderSession(...)`：仅用于不可恢复错误、目标超过会话上限或平台明确不支持 DRC。

会话创建时应区分：

- **会话上限分辨率**：码率阶梯中的最大宽高，例如 1920×1080，用于捕获面容量和编码器会话能力初始化；
- **当前输入分辨率**：例如 1280×720，用于输出面原始格式和输入帧校验。

普通切换状态机建议如下：

```text
RUNNING
  -> 标记切换中，暂不接收新输入帧
  -> 排空已入队输出面原始帧（设置很短的有界等待）
  -> 仅 STREAMOFF 输出面
  -> 仅释放输出面缓冲区
  -> 设置新的 YUV420M 输出格式
  -> 重新申请并排队输出面缓冲区
  -> 仅 STREAMON 输出面
  -> 请求 IDR，确保 SPS/PPS 随 IDR 插入
  -> 接收新尺寸帧
  -> RUNNING
```

捕获面、捕获缓冲区和 DQ 线程在整个过程中保持运行。为避免旧尺寸任务与新码流回调错配，现有 generation 机制应继续保留，并在切换开始时丢弃尚未提交的旧任务。

必须增加运行时能力探测和失败回退：如果输出面 DRC 的任一步失败，立即进入完整会话恢复，不要让半初始化状态继续收帧。升分辨率应仅允许到会话初始上限；在精确的 JetPack/SoC 组合上通过压力测试前，不应宣称支持任意低到高切换。

### 4.2 动态码率和帧率不触发重配

当前封装已支持动态 `setBitrate()`。应增加 `SetFramerate(uint32_t fps_num, uint32_t fps_den)`，在 `SetRates()` 中把 WebRTC 的帧率更新直接传给硬件。NVIDIA 文档明确说明 `setBitrate()` 和 `setFrameRate()` 可在格式设置完成后调用，无须停止流。

为减少频繁 ioctl，可合并小幅更新：

- 码率变化超过 5%～10%，或距离上次应用超过约 200 ms 时更新；
- 帧率整数值改变时更新；
- 每次实际应用后记录目标值、返回码和耗时。

不要用全局最小码率把硬件输出强行钳高到 WebRTC 目标之上。最低码率应作为分辨率阶梯和带宽分配的提示；如果网络目标低于当前分辨率的可用下限，应触发降分辨率，而不是继续超发。

### 4.3 自适应策略：先空间、后时间

动态画面应保持已有的 `kFluid` 内容提示，使 WebRTC 优先保持帧率。推荐策略：

1. 当前档位码率不足、平均 QP 过高或编码丢帧持续 0.5～1 秒：快速下降一档分辨率；
2. 仅当最低分辨率仍不能维持时：30 → 24 → 20 → 15 fps；
3. 网络恢复、目标码率高于上一档启动码率 20%～30%、平均 QP 足够低且状态持续 5～10 秒：上升一档；
4. 每次切换后设置 3～5 秒冷却时间，一次只跨一个档位；
5. 如果 15 fps 的最低档仍无法稳定发送，宁可暂停连续视频并发送低频缩略帧，也不要让队列积压造成数秒延时。

应重新启用 `VideoEncoder::ScalingSettings`，并让“编码队列满导致的本地丢帧”进入 WebRTC 的丢帧/质量反馈。`EmplaceBuffer()` 应返回 `accepted`、`dropped` 或 `error`，上层据此调用适配版本所支持的丢帧通知或至少记录统一指标。

### 4.4 建议的 30 fps H.264 码率阶梯

下面数值是以约 0.06/0.10/0.16 bit-per-pixel-per-frame 推导的初始基线，需要用实际道路、车辆运动、摄像头噪声和光照素材重新标定：

| 分辨率 | 最低可用码率 | 推荐启动码率 | 建议上限 |
| --- | ---: | ---: | ---: |
| 320×180 | 100 kbps | 175 kbps | 300 kbps |
| 480×270 | 250 kbps | 400 kbps | 650 kbps |
| 640×360 | 450 kbps | 700 kbps | 1.2 Mbps |
| 960×540 | 1.0 Mbps | 1.6 Mbps | 2.5 Mbps |
| 960×600 | 1.1 Mbps | 1.8 Mbps | 2.8 Mbps |
| 1280×720 | 1.2 Mbps | 1.6 Mbps | 4.5 Mbps |
| 1280×800 | 1.6 Mbps | 2.4 Mbps | 5.2 Mbps |
| 1440×900 | 2.8 Mbps | 4.2 Mbps | 7.5 Mbps |
| 1920×1080 | 3.8 Mbps | 6.2 Mbps | 10～12 Mbps |
| 1920×1200 | 4.2 Mbps | 7.0 Mbps | 12 Mbps |

高运动场景可把启动码率和上限增加 20%～40%。全局最低目标建议降到 150～250 kbps 的量级，并保证它不高于最低分辨率的可用范围。最终门槛不应只看理论表，而应结合滑动窗口内的实际 QP、编码帧大小、发送队列和丢包反馈。

### 4.5 QP、GOP、码控和低延时参数

| 参数 | 推荐 | 原因与注意事项 |
| --- | --- | --- |
| B 帧 | 保持 0 | 避免重排序等待，降低延时和丢包传播复杂度 |
| 参考帧 | 从 1 开始验证 | 降低编码状态和丢包影响；必须在申请缓冲区前设置 |
| QP 范围 | 初期可不做硬钳制；或从 `[20,46]` 起测 | 当前 `[30,36]` 范围过窄，既限制好网络画质，又阻断 QP 降档 |
| QP 缩放阈值 | 当前以低 32、高 36 作为道路动态画面联调起点 | 缩小原有死区，使好网升档和弱网降档更容易触发；仍需根据每秒 QP 窗口继续校准 |
| 码控 | 极端网络优先 CBR | VBR 峰值突发会增加 pacer 排队；如使用 VBR，峰值需严格约束 |
| VBV | 从 2 帧容量起测，范围 1～3 帧 | 小 VBV 有助于约束突发和延时，但过小会明显损伤复杂画面质量 |
| GOP/IDR | 保留长 GOP，PLI/切换时强制 IDR | 不建议仅为“恢复快”而频繁周期性 IDR，IDR 会产生码率尖峰 |
| SPS/PPS | 保持随 IDR 插入 | 分辨率切换和丢包恢复必需 |
| 编码预设 | 保留 UltraFast 作为低延时基线，对 Fast/Medium 做 A/B | 更慢预设可能提升同码率清晰度，但必须满足每帧 p99 时延预算 |
| 最大性能模式 | 保持开启 | 降低频率波动引起的编码时延抖动，但功耗与温度需要监测 |
| V4L2 输出缓冲数 | 从 2～3 个起测，不建议盲目设为 1 | 当前 4 个缓冲可能放大排队上界；过少会降低吞吐和稳定性 |
| 两遍 CBR | 仅做 A/B 实验 | 可能提高码率准确度/画质，也可能增加硬件负载和时延，不应直接作为默认 |
| Slice/Intra Refresh | 仅在明确丢包收益后启用 | RTP 可分片 NAL，过多 slice 会降低压缩效率；与 IDR/NACK/FEC 策略联调 |

VBV 如果使用 Jetson API 的字节数接口，可从下式估算：

```text
VBV bytes = ceil(帧数 × bitrate_bps / (8 × fps))
```

当前封装要求在申请缓冲区前设置 VBV，所以它不能像码率一样随时更新。应以会话典型码率或档位上限配置，并在目标 JetPack 支持运行时 VBV 初始化参数时再做能力探测，不能假定所有版本都支持。

### 4.6 接收端抖动缓冲

`playout_delay_min_ms=0`、`playout_delay_max_ms=10` 并不会降低 NVENC 编码耗时；它是在要求接收端只保留极小的播放缓冲。对高 RTT、高抖动和突发丢包网络，这可能把可吸收的网络抖动压缩到不足一帧，表现为频繁冻结。

建议取消过窄的强制最大值，让 WebRTC 自适应；如果业务必须限定，可从以下区间实测：

- 极低延时局域网：0～80 ms；
- 兼顾极端网络流畅性的默认值：20～120 ms。

具体值应以“控制操作端允许的最大玻璃到玻璃延时”和抖动分布共同决定，不能只追求配置数字最小。

## 5. 热备编码器兜底方案

如果输出面 DRC 在目标平台上无法稳定工作，推荐使用受控的热备方案，而不是恢复旧的全局编码器池：

1. 每条活跃视频轨道最多维护当前编码器、相邻低一档和相邻高一档三个会话；
2. 设立跨轨道的 NVENC 会话/显存/缓冲区全局预算，使用 LRU 回收；
3. 热备创建后必须实际编码并丢弃一帧，确认 DQ 回调已工作，不能只完成 `Create/Start`；
4. 切换时先让新编码器输出 IDR 和 SPS/PPS，确认码流提交后再停止旧编码器；
5. 如果目标档没有命中热备，不得在实时编码线程同步创建；应继续发送当前档或先临时降低帧率，等待后台准备完成；
6. 记录热备命中率、创建耗时、首帧耗时和资源占用，防止多摄像头/多对端时耗尽硬件资源。

仓库曾经的异步预热实现只保证存在某个 standby，并不保证下一目标分辨率命中；同时它没有通过真实编码帧消除固件首帧开销，也没有全局会话预算。因此只能借鉴“双编码器无缝交接”的思想，不能直接恢复旧代码。

完整 simulcast 不是当前点对点 `rtc_edge` 场景的首选：底层不原生支持 simulcast 时，WebRTC 适配器会创建多个独立编码器并同时产生多路空间流，显著增加 Jetson 编码会话数、功耗和上行带宽。只有在 SFU 能按订阅者选择层、且硬件资源预算明确时才值得采用。

## 6. 分阶段落地建议

### 阶段一：消除切换长卡顿

修改重点：

- `jetson_encoder.h/.cpp`
  - 保存会话上限尺寸和当前输出面尺寸；
  - 实现仅输出面 DRC，保留捕获面和 DQ 线程；
  - 增加动态 `SetFramerate()`；
  - 把完整重启保留为错误回退；
  - 让输入队列操作返回明确的接收/丢弃/错误状态；
  - 为切换各阶段增加微秒级耗时和首个新 IDR 时间指标。
- `jetsonh264_encoder_impl.h/.cpp`
  - 普通尺寸变化调用 DRC，不再同步销毁/创建；
  - `SetRates()` 同时应用码率和帧率；
  - 删除“硬件输出不得低于全局最低码率”的钳制；
  - 保留 generation 隔离旧任务，切换完成后强制关键帧。

阶段一不改变 `vtsrtc` 对外 C ABI。

### 阶段二：恢复有效的带宽/质量自适应

- 为每个分辨率填入真实的 `ResolutionBitrateLimits`；（已完成）
- 重新开启 `ScalingSettings`，修正 QP 范围和阈值关系；（已完成）
- 增加快速降档、慢速升档、冷却和单级切换；
- 把编码器本地丢帧、QP、实际码率和 WebRTC 目标码率纳入统一决策；
- 调整或取消 0～10 ms 的过窄播放延时约束；
- 统一工厂宣告的 H.264 profile 与硬件实际输出。若要启用 High/CABAC 提升压缩效率，必须完成编码、SDP 协商和解码兼容性的端到端测试。

### 阶段三：减少逐帧复制和 CPU 抖动

在不修改公开 C ABI 的前提下，为仓库内部摄像头路径增加原生帧类型：优先让摄像头/CUDA 产出 NV12，并通过 `NvBufSurface`/DMA-BUF 直接送入 V4L2 输出面。WebRTC 仍可保留 I420 回退路径。

该阶段能降低捕获到 QBUF 的时延、CPU 占用和内存带宽，尤其适合 1080p、多路摄像头；但它涉及缓冲区生命周期、跨线程同步、色彩格式和平台隔离，风险高于阶段一，不应与 DRC 修复混在同一个提交中。

## 7. 验证与验收方案

### 7.1 必须补充的时延指标

对每帧记录并关联以下时间点：

- 摄像头采集完成；
- CUDA/颜色转换完成；
- WebRTC I420 复制完成；
- `Encode()` 进入；
- V4L2 `QBUF`；
- 捕获面 `DQBUF`；
- `OnEncodedImage()` 返回。

对分辨率切换单独记录：输出面排空、`STREAMOFF`、缓冲释放、`S_FMT`、缓冲申请、`STREAMON`、首个新尺寸 IDR 回调。统计 p50、p95、p99、最大值、连续丢帧数和最大画面间隔，平均值不足以说明卡顿问题。

### 7.2 网络矩阵

使用[弱网控制工具](jetson_weak_network_test.md)对真实视频素材执行至少以下组合。支持 `netem` 的内核使用排队整形；当前 Orin 内核使用 `iptables` policing 和随机丢包：

- 带宽阶梯：12M → 6M → 3M → 1.5M → 600k → 250k，并逆序恢复；
- 丢包：0%、2%、5%、10%，包括突发丢包；
- RTT：20、100、250 ms；
- 抖动：0、30、80 ms；
- 带宽快速抖动和每 1～3 秒跨档，验证迟滞是否能阻止分辨率乒乓切换。

同时采集目标码率、实际编码码率、发送 pacer 队列、QP、分辨率、帧率、关键帧大小、NACK/PLI、RTT、丢包和接收端冻结时间。

### 7.3 平台与压力测试

- 在实际支持的 JetPack R35/R36、每种目标 Jetson SoC 上分别验证；
- 1080p ↔ 720p ↔ 540p ↔ 360p 循环切换至少 1000 次；
- 验证高到低、低到高、连续快速切换和切换中收到 PLI；
- 验证新分辨率首帧为可独立解码的 IDR，包含 SPS/PPS，且无旧任务回调错配；
- 多摄像头、多 peer 同时运行，检查 NVENC 会话、FD、缓冲区和内存泄漏；
- 注入 `STREAMOFF`、`S_FMT`、缓冲申请和 DQ 超时失败，确认能完整回退且不会死锁。

建议阶段一的验收门槛：

- 正常码率阶梯切换路径不出现编码器对象创建/销毁；
- 30 fps 下切换画面间隔理想值不超过 1 帧，首版可接受上限不超过 2 帧（约 67 ms）；
- 编码复制加硬件编码的 p99 不持续超过约 15 ms，且队列无单调增长；
- CBR 稳定后 1 秒窗口实际码率不长期超过目标的 110%；
- 任何网络条件下，发送队列不会以牺牲端到端延时的方式无限积压。

这些阈值是建议工程目标，最终需结合分辨率、SoC 性能和业务端到端延时预算调整。

## 8. 风险与取舍

- NVIDIA 官方样例明确偏向高到低 DRC；低到高虽可能在初始上限内工作，但必须以目标驱动实测决定是否开放。
- 更慢编码预设、High profile、CABAC、两遍 CBR 能提高同码率画质的潜力，但可能增加负载、时延或兼容风险，应逐项 A/B，不应一次性启用。
- 小 VBV 和少缓冲区降低排队上界，也会让复杂帧更容易升 QP 或丢帧；需要用高速运动、树叶、雨雪、夜间噪声等高熵素材测试。
- 过快升档会造成来回切换和 IDR 码率尖峰，因此降档与升档必须使用不同时间尺度。
- 零复制能进一步降低时延，但实现复杂，应在单会话 DRC 和自适应闭环稳定后单独推进。

## 9. 资料来源

- [NVIDIA Jetson NvVideoEncoder API（JetPack R36.4.3）](https://docs.nvidia.com/jetson/archives/r36.4.3/ApiReference/classNvVideoEncoder.html)
- [NVIDIA 01_video_encode 官方样例说明](https://docs.nvidia.com/jetson/archives/r36.4.3/ApiReference/l4t_mm_01_video_encode.html)
- [NVIDIA Accelerated GStreamer 编码参数说明](https://docs.nvidia.com/jetson/archives/r36.4.4/DeveloperGuide/SD/Multimedia/AcceleratedGstreamer.html)
- [WebRTC VideoEncoder 接口与 ScalingSettings](https://webrtc.googlesource.com/src/+/refs/heads/main/api/video_codecs/video_encoder.h)
- [WebRTC QualityScalerResource](https://webrtc.googlesource.com/src/+/refs/heads/main/video/adaptation/quality_scaler_resource.cc)
- [WebRTC QualityScaler 实现](https://webrtc.googlesource.com/src/+/refs/heads/main/modules/video_coding/utility/quality_scaler.cc)
- [WebRTC 内容提示与 DegradationPreference 映射](https://webrtc.googlesource.com/src/+/refs/heads/main/media/engine/webrtc_video_engine.cc)
- [WebRTC SimulcastEncoderAdapter](https://webrtc.googlesource.com/src/+/refs/heads/main/media/engine/simulcast_encoder_adapter.cc)

## 10. 最终建议

优先完成“**单 NVENC 会话 + 仅输出面 DRC + 动态码率/帧率更新**”，这是消除分辨率切换长卡顿、同时保留低延时能力的关键。随后用真实码率阶梯、QP/丢帧反馈和迟滞机制恢复 WebRTC 空间自适应，再通过 CBR、VBV、预设和 QP 的 A/B 测试提升可用带宽内的清晰度。只有 DRC 在指定平台验证失败时，才启用带资源预算且完成真实首帧预热的热备编码器。
