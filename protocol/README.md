# 协议目录

每个协议目录中的 `schema/` 是协议定义的唯一事实来源，并与生成代码和 C++ codec 一起维护。

- `vehicle/`：车辆驾驶、换档、事务回执和状态反馈协议。
- `vision/`：视觉能力、类别表和检测帧协议。

不同设备类型使用独立的目录、protobuf package 和 DataChannel label，不共用泛化的业务信封。未来增加机械狗协议时，应新建 `quadruped/` 并使用独立版本；在控制器能力和安全约束明确前，不创建占位 schema。

每个协议目录遵循相同布局：

```text
schema/                .proto 与 nanopb .options
generated/             nanopb 自动生成的 C 文件
include/<命名空间>/    对外 C++ codec 头文件
src/                   codec 实现
tests/                 协议测试（存在时）
```

禁止手工修改 `generated/`。修改 schema 后必须使用 nanopb 0.4.9 重新生成并提交对应的 `.pb.h` 与 `.pb.c`：

```sh
python tools/generate_nanopb.py
python tools/generate_nanopb.py --check
```
