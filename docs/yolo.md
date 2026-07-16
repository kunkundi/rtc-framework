# 编译
 xmake b -vy rtc_dual_camera_headless

# 生成 YOLO26n ONNX 模型
先安装 Ultralytics 导出依赖：

```bash
python3 -m pip install -U 'numpy<2' ultralytics onnx onnxslim
```

Jetson/Ubuntu 22.04 上如果看到 `_ARRAY_API not found` 或 `numpy.core.multiarray failed to import`，通常是 `numpy 2.x` 和系统 `matplotlib` 不兼容，重新执行上面的命令把 NumPy 固定到 1.x 即可。

然后让 xmake 调用导出脚本：

```bash
xmake yolo_model
```

默认会通过 Ultralytics 下载 `yolo26n.pt`，并导出：

```text
models/yolo26n.pt
models/yolo26n.onnx
```

默认输入形状为 `[2,3,512,512]`，左右相机图像通过一次 TensorRT enqueue 执行 batch=2 推理。原始 UYVY/YUYV 帧会直接上传 GPU，由 CUDA 同时完成 RGB 转换、letterbox 缩放和 NCHW 排布，不再为 YOLO 在 CPU 上生成两份 I420 临时帧。`512x512` 用于降低双目推理的 GPU 负载。如果需要优先保留小目标精度，可以显式改回 `640`：

```bash
VTSRTC_YOLO_IMGSZ=640 xmake yolo_model
```

如果要使用本地权重：

```bash
VTSRTC_YOLO_SOURCE=/path/to/yolo26n.pt xmake yolo_model
```

如果 Jetson 上安装 PyTorch/Ultralytics 不方便，也可以在开发机上导出 `yolo26n.onnx`，然后放到本仓库的 `models/yolo26n.onnx`。

# YOLO 编译开关

YOLO TensorRT 推理默认开启，正常构建即可：

```bash
xmake b -vy rtc_dual_camera_headless
```

如果当前环境没有 TensorRT，可以显式关闭：

```bash
xmake f --enable_yolo=false
```

启用后会链接 Jetson 系统 TensorRT/CUDA 库，并在构建后把 `models/yolo26n.onnx` 复制到运行目录。程序首次启动会从 ONNX 构建 TensorRT engine，默认缓存为 `models/yolo26n.onnx.trt`；旁边的 `.trt.meta` 会记录 ONNX 内容指纹，后续启动只要模型内容没变就会直接加载缓存。旧的 batch=1 ONNX 模型需要重新执行 `xmake yolo_model` 导出。

# 运行
 xmake r rtc_dual_camera_headless --room zhejianglab

运行时也可以手动指定模型：

```bash
VTSRTC_YOLO_MODEL=/path/to/yolo26n.onnx xmake r rtc_dual_camera_headless --room zhejianglab
```

也可以手动指定 TensorRT engine 缓存路径：

```bash
VTSRTC_YOLO_TRT_ENGINE=/path/to/yolo26n.onnx.trt xmake r rtc_dual_camera_headless --room zhejianglab
```

# 降低视觉处理分辨率

`config/rtc.cfg` 中可以配置 YOLO/OpenCV 消费端的工作帧缩放倍数，不影响 RTC 实际推流分辨率：

```json
"edge": {
    "yolo": {
        "max_fps": 15,
        "processing_downscale": 2
    }
}
```

`processing_downscale` 默认为 `2`，取值范围为 `1..8`，`1` 表示不缩放。该缩放只影响显示拼接工作帧和 OpenCV ORB/RANSAC；YOLO 只检测原始左、右相机单目帧，再按显示端横向抽样/拼接比例把框映射回拼接图坐标。没有原始双目帧时不会回退到拼接工作帧检测。

`max_fps` 默认为 `15`，用于限制 YOLO 消费端的最大处理帧率；设为 `0` 可关闭限制。帧队列深度为 `1`，跳过的旧帧不会积压，因此不会额外增加检测链路延迟。

# 编译并运行
 xmake b -vy rtc_dual_camera_headless && xmake r rtc_dual_camera_headless --room zhejianglab

# 添加第三方库依赖

YOLO 推理路径使用：

- `yolo26n.pt`：Ultralytics 原始权重，只用于 Python 导出。
- `models/yolo26n.onnx`：C++ 启动时输入 TensorRT parser 的模型。
- `models/yolo26n.onnx.trt`：TensorRT engine 缓存，模型更新后会自动重建。
- `models/yolo26n.onnx.trt.meta`：模型内容指纹，避免构建脚本复制 ONNX 后仅因文件时间变化触发重建。
- Jetson TensorRT/CUDA：`libnvinfer`、`libnvinfer_plugin`、`libnvonnxparser`、`libcudart`。

当前 C++ 后处理支持 Ultralytics 常见输出形状：`[1,84,8400]`、`[1,8400,84]` 和端到端结果 `[1,N,6]`。
