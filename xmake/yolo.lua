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

task("yolo_engine")
    set_category("plugin")
    set_menu({
        usage = "xmake yolo_engine",
        description = "Prebuild models/yolo26n.onnx.trt with TensorRT trtexec"
    })

    on_run(function()
        local python = os.getenv("PYTHON") or "python3"
        local script = path.join(os.projectdir(), "tools", "build_yolo_engine.py")
        local model_path = os.getenv("VTSRTC_YOLO_MODEL") or
            path.join(os.projectdir(), "models", "yolo26n.onnx")
        local engine_path = os.getenv("VTSRTC_YOLO_TRT_ENGINE") or
            (model_path .. ".trt")
        local arguments = {
            script,
            "--model", model_path,
            "--engine", engine_path,
            "--workspace", os.getenv("VTSRTC_YOLO_TRT_WORKSPACE") or "1G",
            "--optimization-level", os.getenv("VTSRTC_YOLO_TRT_OPT_LEVEL") or "3",
            "--progress-interval", os.getenv("VTSRTC_YOLO_TRT_PROGRESS_INTERVAL") or "0.1"
        }
        local trtexec = os.getenv("TRTEXEC")
        if trtexec then
            table.insert(arguments, "--trtexec")
            table.insert(arguments, trtexec)
        end
        os.execv(python, arguments)
    end)
task_end()
