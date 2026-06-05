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

如果要使用本地权重：

```bash
VTSRTC_YOLO_SOURCE=/path/to/yolo26n.pt xmake yolo_model
```

如果 Jetson 上安装 PyTorch/Ultralytics 不方便，也可以在开发机上导出 `yolo26n.onnx`，然后放到本仓库的 `models/yolo26n.onnx`。

# 开启 YOLO 编译

```bash
xmake f --enable_yolo=true
xmake b -vy rtc_dual_camera_headless
```

启用后会链接 ONNX Runtime，并在构建后把 `models/yolo26n.onnx` 复制到运行目录。

# 运行
 xmake r rtc_dual_camera_headless --room zhejianglab

运行时也可以手动指定模型：

```bash
VTSRTC_YOLO_MODEL=/path/to/yolo26n.onnx xmake r rtc_dual_camera_headless --room zhejianglab
```

# 编译并运行
 xmake b -vy rtc_dual_camera_headless && xmake r rtc_dual_camera_headless --room zhejianglab

# 添加第三方库依赖

YOLO 推理路径使用：

- `yolo26n.pt`：Ultralytics 原始权重，只用于 Python 导出。
- `models/yolo26n.onnx`：C++ 实际加载的模型。
- `onnxruntime 1.22.0`：通过 xmake 包管理，在 `--enable_yolo=true` 时启用。

当前 C++ 后处理支持 Ultralytics 常见输出形状：`[1,84,8400]`、`[1,8400,84]` 和端到端结果 `[1,N,6]`。
