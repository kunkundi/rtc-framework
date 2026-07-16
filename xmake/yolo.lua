task("yolo_model")
    set_category("plugin")
    set_menu({
        usage = "xmake yolo_model",
        description = "Download yolo26n.pt with Ultralytics and export models/yolo26n.onnx"
    })

    on_run(function()
        local python = os.getenv("PYTHON") or "python3"
        local script = path.join(os.projectdir(), "tools", "export_yolo_model.py")
        os.execv(python, {
            script,
            "--model", os.getenv("VTSRTC_YOLO_SOURCE") or "yolo26n.pt",
            "--out-dir", path.join(os.projectdir(), "models"),
            "--imgsz", os.getenv("VTSRTC_YOLO_IMGSZ") or "512"
        })
    end)
task_end()
