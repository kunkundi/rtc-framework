## RTC Headless 自启动服务

本文说明如何使用 systemd 编写 `rtc_dual_camera_headless` 的自启动服务，以及如何启动、关闭、查看状态和排查问题。

当前机器上的进程启动链路如下：

```text
systemd
└─ rtc-startup.service
   └─ /bin/bash /home/nvidia/startup.sh
      └─ /home/nvidia/.local/bin/xmake r rtc_dual_camera_headless --room zhejianglab
         └─ build/runtime/rtc_dual_camera_headless --room zhejianglab
```

### 1. 编写启动脚本

创建 `/home/nvidia/startup.sh`：

```bash
#!/bin/bash

export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin:/home/nvidia/.local/bin
export HOME=/home/nvidia

cd /home/nvidia/workspace/rtc-solutions

echo "PATH=$PATH" >> /tmp/rtc.log
echo "whoami=$(whoami)" >> /tmp/rtc.log

/home/nvidia/.local/bin/xmake r rtc_dual_camera_headless --room zhejianglab >> /tmp/rtc.log 2>&1
```

赋予可执行权限：

```bash
chmod +x /home/nvidia/startup.sh
```

如果需要修改房间名，调整最后一行的 `--room zhejianglab` 即可。

### 2. 编写 systemd 服务

创建 `/etc/systemd/system/rtc-startup.service`：

```ini
[Unit]
Description=RTC Auto Start Service
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=nvidia
WorkingDirectory=/home/nvidia/workspace/rtc-solutions

ExecStart=/bin/bash /home/nvidia/startup.sh

Environment="PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin:/home/nvidia/.local/bin"
Environment="HOME=/home/nvidia"

Restart=on-failure
RestartSec=5

[Install]
WantedBy=multi-user.target
```

修改 service 文件后，需要重新加载 systemd 配置：

```bash
sudo systemctl daemon-reload
```

### 3. 启用开机自启动

```bash
sudo systemctl enable rtc-startup.service
```

启用后，系统进入 `multi-user.target` 时会自动启动该服务。

取消开机自启动：

```bash
sudo systemctl disable rtc-startup.service
```

### 4. 启动、关闭和重启

立即启动服务：

```bash
sudo systemctl start rtc-startup.service
```

关闭服务：

```bash
sudo systemctl stop rtc-startup.service
```

重启服务：

```bash
sudo systemctl restart rtc-startup.service
```

查看服务状态：

```bash
systemctl status rtc-startup.service
```

查看服务内的进程：

```bash
systemctl status rtc-startup.service
pstree -alps $(systemctl show -p MainPID --value rtc-startup.service)
```

### 5. 查看日志

查看 systemd 日志：

```bash
journalctl -u rtc-startup.service --no-pager
```

实时跟踪 systemd 日志：

```bash
journalctl -u rtc-startup.service -f
```

查看启动脚本写入的日志：

```bash
tail -n 100 /tmp/rtc.log
tail -f /tmp/rtc.log
```

### 6. 常见排查命令

确认服务文件内容：

```bash
systemctl cat rtc-startup.service
```

确认服务是否已启用：

```bash
systemctl is-enabled rtc-startup.service
```

确认服务当前状态：

```bash
systemctl is-active rtc-startup.service
```

确认进程父子关系：

```bash
ps -o pid,ppid,pgid,sid,user,lstart,tty,stat,cmd -p <PID>
pstree -alps <PID>
```

确认服务所属 cgroup：

```bash
cat /proc/<PID>/cgroup
```

### 7. 注意事项

1. service 中使用 `User=nvidia`，因此程序会以 `nvidia` 用户身份运行。
2. systemd 启动时环境变量通常不完整，建议在 service 和 `startup.sh` 中显式设置 `PATH` 和 `HOME`。
3. `xmake r` 会先构建或检查目标，再启动 `rtc_dual_camera_headless`。如果希望跳过 xmake，也可以在脚本中直接运行 `build/runtime/rtc_dual_camera_headless`。
4. 修改 `/etc/systemd/system/rtc-startup.service` 后，一定要执行 `sudo systemctl daemon-reload`。
5. `sudo systemctl stop rtc-startup.service` 会停止该服务 cgroup 下的子进程，包括 `xmake` 和 `rtc_dual_camera_headless`。
