# YOLO 开发指南

默认使用 YOLO26n、TensorRT FP16 和 `[2,3,512,512]` 输入。左右 UYVY/YUYV 由 CUDA 直转 RGB NCHW，一次 batch=2 enqueue 输出端到端 `[2,N,6]` 检测结果。

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

构建后会把 ONNX 以及已存在的 `.trt`/`.trt.meta` 复制到运行目录。缺少 engine 时，程序会在首帧到达后构建并缓存。

## 运行配置

```json
{
    "edge": {
        "yolo": {
            "enabled": true,
            "max_fps": 15,
            "processing_downscale": 2
        }
    }
}
```

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
