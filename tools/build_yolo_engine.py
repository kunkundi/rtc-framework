#!/usr/bin/env python3
"""在目标 NVIDIA 设备上预构建 YOLO TensorRT engine。"""

import argparse
import os
import signal
import shutil
import subprocess
import sys
import threading
import time
from pathlib import Path


METADATA_VERSION = "vtsrtc_yolo_trt_cache_v1"
FNV_OFFSET_BASIS = 1469598103934665603
FNV_PRIME = 1099511628211


def parse_args():
    parser = argparse.ArgumentParser()
    parser.add_argument("--model", required=True)
    parser.add_argument("--engine", required=True)
    parser.add_argument("--trtexec")
    parser.add_argument("--workspace", default="1G")
    parser.add_argument("--optimization-level", type=int, default=3)
    parser.add_argument("--progress-interval", type=float, default=0.1)
    return parser.parse_args()


def resolve_trtexec(explicit_path):
    candidates = [
        explicit_path,
        os.environ.get("TRTEXEC"),
        shutil.which("trtexec"),
        "/usr/src/tensorrt/bin/trtexec",
    ]
    for candidate in candidates:
        if candidate and Path(candidate).is_file() and os.access(candidate, os.X_OK):
            return str(Path(candidate).resolve())
    raise RuntimeError(
        "trtexec not found; set TRTEXEC or install TensorRT tools"
    )


def model_fingerprint(model_path):
    size = 0
    value = FNV_OFFSET_BASIS
    with model_path.open("rb") as source:
        while True:
            block = source.read(1024 * 1024)
            if not block:
                break
            size += len(block)
            for byte in block:
                value ^= byte
                value = (value * FNV_PRIME) & 0xFFFFFFFFFFFFFFFF
    return size, value


def write_metadata(model_path, engine_path):
    size, fingerprint = model_fingerprint(model_path)
    metadata_path = Path(str(engine_path) + ".meta")
    temporary_path = Path(str(metadata_path) + ".tmp")
    temporary_path.write_text(
        "{} {} {}\n".format(METADATA_VERSION, size, fingerprint),
        encoding="ascii",
    )
    temporary_path.replace(metadata_path)
    return metadata_path


def format_elapsed(seconds):
    hours, remainder = divmod(max(0, int(seconds)), 3600)
    minutes, seconds = divmod(remainder, 60)
    if hours > 0:
        return "{}:{:02d}:{:02d}".format(hours, minutes, seconds)
    return "{:02d}:{:02d}".format(minutes, seconds)


def run_with_progress(command, interval_seconds):
    process = subprocess.Popen(
        command,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
    )
    started_at = time.monotonic()
    indicators = "|/-\\"
    indicator_index = 0
    terminal_output = sys.stdout.isatty()
    output_lock = threading.Lock()

    def clear_progress_line():
        if terminal_output:
            sys.stdout.write("\r\033[2K")

    def forward_process_output():
        if process.stdout is None:
            return
        for line in process.stdout:
            with output_lock:
                clear_progress_line()
                sys.stdout.write(line)
                sys.stdout.flush()

    output_thread = threading.Thread(target=forward_process_output)
    output_thread.start()
    while True:
        try:
            return_code = process.wait(timeout=interval_seconds)
            output_thread.join()
            with output_lock:
                clear_progress_line()
                sys.stdout.flush()
            return return_code
        except subprocess.TimeoutExpired:
            elapsed = format_elapsed(time.monotonic() - started_at)
            indicator = indicators[indicator_index % len(indicators)]
            indicator_index += 1
            status = (
                "TensorRT engine building [{}] elapsed={}, process active".format(
                    indicator, elapsed
                )
            )
            with output_lock:
                if terminal_output:
                    clear_progress_line()
                    sys.stdout.write(status)
                    sys.stdout.flush()
                else:
                    print(status, flush=True)
        except KeyboardInterrupt:
            process.send_signal(signal.SIGINT)
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
            output_thread.join()
            with output_lock:
                clear_progress_line()
                sys.stdout.flush()
            raise


def main():
    args = parse_args()
    model_path = Path(args.model).resolve()
    engine_path = Path(args.engine).resolve()
    if not model_path.is_file():
        print("YOLO ONNX model not found: {}".format(model_path), file=sys.stderr)
        print("Run `xmake yolo_model` first.", file=sys.stderr)
        return 2
    if args.optimization_level < 0 or args.optimization_level > 5:
        print("optimization level must be in 0..5", file=sys.stderr)
        return 2
    if args.progress_interval < 0.05 or args.progress_interval > 3600:
        print("progress interval must be in 0.05..3600 seconds", file=sys.stderr)
        return 2

    try:
        trtexec = resolve_trtexec(args.trtexec)
    except RuntimeError as exc:
        print(str(exc), file=sys.stderr)
        return 2

    engine_path.parent.mkdir(parents=True, exist_ok=True)
    temporary_engine = Path(str(engine_path) + ".tmp")
    try:
        temporary_engine.unlink()
    except FileNotFoundError:
        pass

    command = [
        trtexec,
        "--onnx={}".format(model_path),
        "--saveEngine={}".format(temporary_engine),
        "--fp16",
        "--memPoolSize=workspace:{}".format(args.workspace),
        "--builderOptimizationLevel={}".format(args.optimization_level),
        "--skipInference",
    ]
    print("Building TensorRT engine: {}".format(engine_path), flush=True)
    print("Command: {}".format(" ".join(command)), flush=True)
    try:
        return_code = run_with_progress(command, args.progress_interval)
    except KeyboardInterrupt:
        print("TensorRT engine build canceled", file=sys.stderr)
        return 130
    if return_code != 0 or not temporary_engine.is_file():
        print("TensorRT engine build failed", file=sys.stderr)
        return return_code or 3

    temporary_engine.replace(engine_path)
    metadata_path = write_metadata(model_path, engine_path)
    print("TensorRT engine: {}".format(engine_path))
    print("TensorRT metadata: {}".format(metadata_path))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
