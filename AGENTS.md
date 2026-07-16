# 仓库 Agent 指南

本文件适用于整个仓库。如果子目录中存在更具体的 `AGENTS.md`，则该文件对相应子目录优先生效。

## 工程概览

`rtc-framework` 是基于 Google WebRTC 开发的 C++14 实时通信工程，主要包括：

- `signaling-server/`：HTTP 房间管理和 WebSocket 信令服务。
- `vtsrtc/`：可复用的 RTC 动态库，通过 `c_rtc.h` 提供稳定的 C ABI。
- `client/apps/`：ImGui、摄像头、接收端和边缘端可执行程序入口。
- `client/modules/`：headless、边缘端、双摄像头、视觉和车辆业务的可复用客户端模块。
- `client/protocols/`：基于 nanopb 的车辆控制与视觉检测协议、生成代码和编解码库。
- `docs/`：信令、视觉检测元数据和车辆控制协议文档。
- `xmake/`：构建选项、依赖辅助函数和目标定义。

WebRTC 和 NVIDIA SDK 等大型二进制依赖通过脚本单独下载，不得提交到仓库。除非任务明确要求修改第三方代码，否则不要编辑 `third_party/` 下的文件。

## 构建与测试

在仓库根目录使用 xmake。开发过程中优先构建受影响的最小目标。

Windows Release 构建与安装：

```powershell
.\run_xmake.bat
```

Linux Release 构建与安装：

```sh
./run_xmake.sh
```

Jetson/aarch64 Release 构建与安装：

```sh
./run_xmake_aarch64.sh
```

常用的局部构建命令：

```powershell
xmake require -y
xmake f -m release --enable_cuda=n
xmake b <target>
xmake r <binary-target>
xmake b rtc_vehicle_control_protocol_tests
xmake r rtc_vehicle_control_protocol_tests
```

Linux 构建脚本要求系统已安装 `libasio-dev`，或者通过 `ASIO_PATH` 指定 standalone Asio。摄像头、CUDA、TensorRT、NVENC 和 Jetson 目标需要对应的硬件与 SDK。如果无法完成全量构建，应构建并测试受影响的最小目标，并在交付说明中写明缺失的环境依赖。

## 编码约定

- 所有新增生产代码必须兼容 C++14。
- 遵循当前文件已有的代码风格。较新的代码通常使用两个空格缩进，较早的 WebRTC 封装代码可能使用制表符。
- 修改应保持聚焦，不要进行无关的格式化或重构。
- 优先复用 `xmake/` 中已有的辅助函数和目标配置，不要重复实现平台检测或依赖配置。
- 可执行程序入口放在 `client/apps/`，可复用代码放在 `client/modules/`；应用不得拥有其他应用需要直接引用的公共实现。
- 模块公共头文件放在 `include/<命名空间>/`，跨模块包含应使用 `rtc_headless/...`、`rtc_edge/...`、`rtc_dual_camera/...`、`rtc_vision/...` 或 `rtc_vehicle/...` 前缀。
- 处理 JSON 和 protobuf 时使用结构化 API，不要手工拼接线协议。
- 代码注释必须使用中文。仅为不明显的行为、协议约束和安全要求添加注释；API 名称、协议字段、标准术语和第三方代码中的原文可以保留英文。
- 不要为了翻译而修改第三方代码或自动生成代码中的注释。
- 工作区可能包含用户尚未提交的修改。不得重置、覆盖或撤销与当前任务无关的改动。

## Git 提交规范

- Commit message 必须使用中文描述，类型和作用域标识保持英文小写。
- 使用 Google/Angular 提交风格：`<type>(<scope>): <中文摘要>`；作用域可省略。
- `type` 只能使用 `feat`、`fix`、`docs`、`style`、`refactor`、`perf`、`test`、`build`、`ci`、`chore` 或 `revert`。
- 摘要应简洁、明确，使用动宾结构，不加句号，不描述无关修改。
- 需要正文时，在摘要后空一行，用中文说明修改原因、实现方式和影响。破坏性变更在页脚使用 `BREAKING CHANGE: <中文说明>`。
- 一个提交只包含一个逻辑改动，不要把无关格式化、重构或生成文件更新混入同一提交。

示例：

```text
feat(control): 增加车辆控制协议编解码

使用 nanopb 编码实时驾驶命令和档位事务，并增加接收端看门狗校验。
```

## ABI 与平台规则

- `vtsrtc` 对外提供 C ABI。除非任务已经明确兼容性方案并包含 ABI 测试，否则不得修改导出的结构体、枚举、函数签名、符号表或所有权规则。
- Windows、通用 Linux 和 Jetson/aarch64 分支都应保持可构建。平台专用的头文件、库、CUDA 源文件和编译参数必须在 xmake 目标中正确隔离。
- 客户端目标应使用 `vtsrtc_add_client_dependency()`，以同时兼容预编译动态库和源码构建。
- 不得提交生成的二进制文件、构建产物、下载的模型、SDK 压缩包、日志或本地配置缓存。

## Protobuf 与生成代码

工程使用 nanopb 0.4.9。`client/protocols/*/schema/` 下的 `.proto` 和 `.options` 文件是协议定义的唯一事实来源。

- 不得手工修改生成的 `.pb.h` 或 `.pb.c` 文件。
- 修改 schema 后必须重新生成并提交对应的两个生成文件。
- 所有字符串和重复字段都必须在对应的 `.options` 文件中设置静态上限。
- 不得复用 protobuf 字段编号。v1 协议的兼容修改只能新增可选字段。
- 解码器必须拒绝错误魔数、不支持的主版本、类型与载荷不匹配、未知的必需枚举、非有限浮点数以及超出文档范围的值。

生成示例：

```powershell
python tools/generate_nanopb.py
python tools/generate_nanopb.py --check
```

## 车辆控制安全规则

车辆控制协议当前仅支持前进、后退、停止，方向盘左转、右转、回中，归一化油门与刹车，以及前进档、倒档和空档。

- 实时驾驶帧通过 `vehicle.control.v1` 发送，通道应设置为乱序且不重传。对于驾驶输入，最新状态比旧状态更有价值。
- 档位事务及其回执通过有序、可靠的 `vehicle.event.v1` 通道发送。
- 状态反馈通过 `vehicle.state.v1` 通道发送。
- 使用 `RtcSendData` 向指定对端发送控制数据，禁止广播控制指令。
- 必须保留二进制载荷的准确长度，包括其中可能存在的零字节。
- 未经明确的安全评审，不得放宽序号校验、有限值与范围校验，也不得延长默认的 300 ms 接收端看门狗。
- RTC 发送成功仅表示数据已在本地入队，不代表设备已经执行。设备执行结果必须通过协议回执或状态确认。
- 网络控制链路不是经过安全认证的控制通道。本地制动、限位、硬件急停和电机禁用逻辑始终拥有最高优先级。

## 验证要求

- 对编解码、协议、生命周期和安全相关修改增加或更新聚焦测试。
- 修改 protobuf 时，应覆盖编码/解码往返测试和固定黄金字节测试。
- 根据修改范围覆盖畸形或截断消息、未知枚举、非法范围、NaN/Inf、重复或乱序序号以及看门狗超时。
- 交付前运行 `git diff --check`。
- 明确说明实际运行过的目标和测试；如果平台或依赖限制阻止了更广泛的验证，也必须如实说明。
