#!/usr/bin/env python3
"""Download/export an Ultralytics YOLO model for the C++ ONNX Runtime path."""

import argparse
import shutil
import sys
from pathlib import Path


def remove_generated_copy(path):
    try:
        path.unlink()
    except FileNotFoundError:
        pass


def parse_args():
    parser = argparse.ArgumentParser()
    parser.add_argument("--model", default="yolo26n.pt")
    parser.add_argument("--out-dir", default="models")
    parser.add_argument("--imgsz", type=int, default=512)
    parser.add_argument("--opset", type=int, default=17)
    parser.add_argument("--no-simplify", action="store_true")
    return parser.parse_args()


def main():
    args = parse_args()
    try:
        from ultralytics import YOLO
    except Exception as exc:
        print("Ultralytics is required to download/export YOLO weights.", file=sys.stderr)
        print("Install it with: python3 -m pip install -U ultralytics onnx onnxslim", file=sys.stderr)
        if "numpy.core.multiarray" in str(exc) or "_ARRAY_API" in str(exc):
            print("Your Python environment likely has NumPy 2.x with NumPy-1.x native modules.", file=sys.stderr)
            print("Fix it with: python3 -m pip install --user 'numpy<2'", file=sys.stderr)
        print("Import error: {}".format(exc), file=sys.stderr)
        return 2

    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    try:
        model = YOLO(args.model)
        exported = model.export(
            format="onnx",
            imgsz=args.imgsz,
            batch=2,
            opset=args.opset,
            nms=True,
            simplify=not args.no_simplify,
            dynamic=False,
        )
    except Exception as exc:
        print("Failed to export {} to ONNX.".format(args.model), file=sys.stderr)
        print("Make sure your Ultralytics package supports this model, or pass a local .pt with VTSRTC_YOLO_SOURCE.", file=sys.stderr)
        print("Export error: {}".format(exc), file=sys.stderr)
        return 3

    exported_path = Path(exported)
    target_onnx = out_dir / (Path(args.model).stem + ".onnx")
    if exported_path.resolve() != target_onnx.resolve():
        shutil.copy2(exported_path, target_onnx)
        if exported_path.parent.resolve() == Path.cwd().resolve():
            remove_generated_copy(exported_path)

    source_pt = Path(args.model)
    if source_pt.is_file():
        target_pt = out_dir / source_pt.name
        if source_pt.resolve() != target_pt.resolve():
            shutil.copy2(source_pt, target_pt)
    else:
        downloaded_pt = Path.cwd() / Path(args.model).name
        if downloaded_pt.is_file():
            target_pt = out_dir / downloaded_pt.name
            if downloaded_pt.resolve() != target_pt.resolve():
                shutil.copy2(downloaded_pt, target_pt)
                if downloaded_pt.parent.resolve() == Path.cwd().resolve():
                    remove_generated_copy(downloaded_pt)

    print("Exported ONNX model: {}".format(target_onnx))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
