#!/usr/bin/env python3

import argparse
import os
import runpy
import shutil
import sys
import tempfile
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[1]
PROTOCOLS = {
    "control": (
        PROJECT_ROOT / "protocol" / "control" / "schema",
        PROJECT_ROOT / "protocol" / "control" / "generated",
        "rtc_control.proto",
    ),
    "vision": (
        PROJECT_ROOT / "protocol" / "vision" / "schema",
        PROJECT_ROOT / "protocol" / "vision" / "generated",
        "vision_detection.proto",
    ),
}


def find_generator():
    configured = os.environ.get("NANOPB_GENERATOR")
    if configured:
        path = Path(configured).expanduser().resolve()
        if path.is_file():
            return path
        raise RuntimeError("NANOPB_GENERATOR 指向的文件不存在：{}".format(path))

    command = shutil.which("nanopb_generator.py")
    if command:
        return Path(command).resolve()

    roots = [
        Path.home() / ".xmake" / "packages" / "n" / "nanopb" / "0.4.9",
        Path.home() / ".local" / "share" / "xmake" / "packages" / "n" / "nanopb" / "0.4.9",
    ]
    local_app_data = os.environ.get("LOCALAPPDATA")
    if local_app_data:
        roots.append(
            Path(local_app_data)
            / ".xmake"
            / "packages"
            / "n"
            / "nanopb"
            / "0.4.9"
        )

    candidates = []
    for root in roots:
        if root.is_dir():
            candidates.extend(root.glob("*/bin/nanopb_generator.py"))
    if not candidates:
        raise RuntimeError(
            "未找到 nanopb_generator.py，请先运行 xmake require -y，"
            "或设置 NANOPB_GENERATOR"
        )
    return max(candidates, key=lambda path: path.stat().st_mtime)


def install_protobuf_compatibility():
    from google.protobuf import message_factory, reflection

    if not hasattr(reflection, "MakeClass"):
        reflection.MakeClass = message_factory.GetMessageClass


def normalize_generated_file(path):
    content = path.read_bytes().replace(b"\r\n", b"\n").rstrip(b"\n") + b"\n"
    path.write_bytes(content)


def normalized_content(path):
    return path.read_bytes().replace(b"\r\n", b"\n").rstrip(b"\n") + b"\n"


def generate_protocol(generator, schema_dir, output_dir, proto_name):
    output_dir.mkdir(parents=True, exist_ok=True)
    previous_argv = sys.argv
    sys.argv = [
        str(generator),
        "--error-on-unmatched",
        "-I",
        str(schema_dir),
        "-D",
        str(output_dir),
        str(schema_dir / proto_name),
    ]
    try:
        runpy.run_path(str(generator), run_name="__main__")
    finally:
        sys.argv = previous_argv

    stem = Path(proto_name).stem
    normalize_generated_file(output_dir / (stem + ".pb.h"))
    normalize_generated_file(output_dir / (stem + ".pb.c"))


def verify_protocol(generated_dir, repository_dir, proto_name):
    stem = Path(proto_name).stem
    for suffix in (".pb.h", ".pb.c"):
        name = stem + suffix
        generated = normalized_content(generated_dir / name)
        repository = normalized_content(repository_dir / name)
        if generated != repository:
            raise RuntimeError("生成文件与仓库不一致：{}".format(repository_dir / name))


def parse_args():
    parser = argparse.ArgumentParser(description="生成或校验仓库 nanopb C 文件")
    parser.add_argument(
        "--protocol",
        choices=("all", "control", "vision"),
        default="all",
        help="选择要处理的协议，默认为 all",
    )
    parser.add_argument(
        "--check",
        action="store_true",
        help="在临时目录生成并与仓库文件比较，不修改仓库",
    )
    return parser.parse_args()


def main():
    args = parse_args()
    generator = find_generator()
    install_protobuf_compatibility()
    selected = PROTOCOLS if args.protocol == "all" else {
        args.protocol: PROTOCOLS[args.protocol]
    }

    if args.check:
        with tempfile.TemporaryDirectory(prefix="rtc-framework-nanopb-") as temp:
            temp_root = Path(temp)
            for name, (schema_dir, repository_dir, proto_name) in selected.items():
                generated_dir = temp_root / name
                generate_protocol(generator, schema_dir, generated_dir, proto_name)
                verify_protocol(generated_dir, repository_dir, proto_name)
                print("校验通过：{}".format(name))
        return

    for name, (schema_dir, output_dir, proto_name) in selected.items():
        generate_protocol(generator, schema_dir, output_dir, proto_name)
        print("生成完成：{}".format(name))


if __name__ == "__main__":
    main()
