# YOLO 开发指南

正式应用支持在 `rtc_console` 或 `rtc_edge` 执行 YOLO26n 识别，默认在 console 运行。

## 选择运行模式

在两端各自使用的 `rtc.cfg` 根对象中设置相同的 `yolo.mode`，修改后重启两端应用：

```json
{
    "yolo": {
        "mode": "console"
    }
}
```

| `yolo.mode` | Edge 行为 | Console 行为 |
| --- | --- | --- |
| `console`（默认） | 采集并发送视频，不启动 YOLO | 对接收解码帧执行 ONNX Runtime CPU 推理，忽略 edge 检测元数据 |
| `edge` | 对原始双目帧执行 TensorRT 推理，通过 `vision.detect.v1` 发送结果 | 不加载本地模型，仅接收并叠加 edge 检测结果 |
| `disabled` | 不启动 YOLO | 不启动本地推理，也不显示 edge 检测结果 |

切换到边缘端只需将两端的 `mode` 改为 `edge`。Edge 模式沿用双目融合、距离估计和跟踪平滑链路，目前仅检测双目 `stereo` 视频；console 模式支持双目和环视画面。Edge 模式需要部署支持 CUDA/TensorRT 的 edge 构建及对应模型/engine；console 本机无需模型即可接收检测结果。

Console 设置页的“目标检测 → YOLO 运行位置”也可选择模式，保存到本机的 `rtc.cfg`。它不会远程修改 edge 配置，部署时仍需同步两端的模式。检测帧率和模型等参数按运行端分别配置。

共享模式优先于旧版 `console.yolo.enabled` 和 `edge.yolo.enabled`。未设置根级 `yolo` 时保留旧行为：console 的 `enabled` 默认 `true`，设为 `false` 时只接收 edge 元数据；edge 的 `enabled` 决定是否在本端推理。新配置不需要这两个开关；保存 console 设置会写入共享模式并移除旧 console 开关。无效的模式值会阻止应用启动。

## Console 本地识别

```sh
xmake require -y
xmake b rtc_console
xmake r rtc_console
xmake b rtc_console_local_yolo_tests
xmake r rtc_console_local_yolo_tests
```

上述命令适用于 Windows 和 Linux。需要对应平台的 WebRTC 和 console 图形依赖；xmake 下载 ONNX Runtime 1.17.1 和 libyuv。构建与安装会复制 ONNX Runtime DLL/共享库及已有的 `models/yolo26n.onnx`，模型仍需通过下文导出步骤生成，不提交二进制依赖。

在 `config/console.cfg` 中配置：

```json
{
    "yolo": {
        "max_fps": 15,
        "model": ""
    }
}
```

通过 `rtc.cfg` 内嵌配置时，上述内容放入 `console` 对象；运行位置仍在根级 `yolo.mode` 中设置。`max_fps` 范围为 `1..240`，是每路接收流的上限，实际速率受 CPU 和流数量限制。`model` 留空时使用 `VTSRTC_YOLO_MODEL` 或默认模型搜索路径；非空时指定 ONNX 路径。

接收线程仅提交每路最新帧，独立工作线程推理，不阻塞显示线程。双目左右画面和环视四格分别识别后映射回整帧坐标。固定 batch=1/2 模型均支持；batch=2 模型复制当前子画面输入并读取第一组结果。关闭房间时丢弃待处理帧和旧会话结果。模型加载失败会阻止 console 初始化，推理失败写入应用日志。

此路径不执行 edge 的 ORB/RANSAC 双目融合、距离估计或跟踪平滑，交叉显示仍由已有 UI 完成。自定义非 COCO 模型应同步调整类别名称表。

Edge TensorRT 路径使用 `[2,3,512,512]` 输入，左右 UYVY/YUYV 由 CUDA 直转 RGB NCHW，一次 batch=2 enqueue 输出端到端 `[2,N,6]` 检测结果。

## 导出 ONNX

```bash
python3 -m pip install -U 'numpy<2' ultralytics onnx onnxslim
xmake yolo_model
```

默认生成 `models/yolo26n.pt` 和 `models/yolo26n.onnx`。自定义权重或输入尺寸：

```bash
VTSRTC_YOLO_SOURCE=/path/to/model.pt VTSRTC_YOLO_IMGSZ=640 xmake yolo_model
```

Jetson 上遇到 NumPy ABI 错误时，确认 NumPy 保持在 1.x。ONNX 可以在开发机导出后复制到 `models/`。

## 预构建 TensorRT engine

在目标 Orin NX 上执行：

```bash
xmake yolo_engine
```

默认使用 FP16、1 GiB workspace 和优化等级 3，生成 `models/yolo26n.onnx.trt` 和 `.trt.meta`。

`trtexec` 输出直接转发到终端，tactic profiling 期间每 `0.1` 秒刷新同一行心跳。JetPack、TensorRT、ONNX、batch 或输入尺寸变化后必须重建 engine。

## 构建与运行

Edge 模式需要在目标 Linux/Jetson 环境保留 CUDA 配置并启用 TensorRT 路径（`xmake f --enable_yolo=y`）。未编译该路径的 edge 程序选择 `edge` 模式时会报错，不会静默切换推理位置。

```bash
xmake b rtc_edge
xmake r rtc_edge --config rtc.cfg
```

```bash
xmake b rtc_dual_camera_headless
xmake r rtc_dual_camera_headless --room zhejianglab
```

```bash
xmake f --enable_yolo=false
```

上面的选项只关闭 edge TensorRT 构建路径，不关闭 console ONNX Runtime 推理。

构建后会把 ONNX 以及已存在的 `.trt`/`.trt.meta` 复制到运行目录。缺少 engine 时，程序会在首帧到达后构建并缓存。

## 运行配置

```json
{
    "yolo": {
        "mode": "edge"
    },
    "edge": {
        "yolo": {
            "max_fps": 15,
            "processing_downscale": 2
        }
    }
}
```

以上为 `rtc.cfg` 内嵌示例；使用独立 `edge.cfg` 时，只把 `edge` 对象的内容放入该文件，根级 `yolo.mode` 保留在 `rtc.cfg`。Console 端也设置 `yolo.mode = "edge"`。

- `max_fps`：`0` 表示不限制，取值范围 `0..240`。
- `processing_downscale`：只影响 OpenCV ORB/RANSAC 双目融合，不改变 YOLO 输入，取值范围 `1..8`。

## 环境变量

| 变量 | 用途 | 默认值 |
| --- | --- | --- |
| `VTSRTC_YOLO_SOURCE` | Ultralytics 权重 | `yolo26n.pt` |
| `VTSRTC_YOLO_IMGSZ` | ONNX 输入尺寸 | `512` |
| `VTSRTC_YOLO_MODEL` | ONNX 路径 | `models/yolo26n.onnx` |
| `VTSRTC_YOLO_TRT_ENGINE` | engine 路径 | `<model>.trt` |
| `VTSRTC_YOLO_TRT_WORKSPACE` | `trtexec` workspace | `1G` |
| `VTSRTC_YOLO_TRT_OPT_LEVEL` | TensorRT 优化等级 | `3` |
| `VTSRTC_YOLO_TRT_PROGRESS_INTERVAL` | 心跳刷新秒数 | `0.1` |
| `TRTEXEC` | `trtexec` 可执行文件 | 自动查找 |

## 约束

- `.onnx` 是模型事实来源；`.trt` 是设备专用缓存；`.trt.meta` 保存 ONNX 指纹。
- 模型和 engine 均为本地产物，不得提交到 Git。
- C++ 解析器支持 `[B,84,N]`、`[B,N,84]` 和端到端 `[B,N,6]` 输出。
