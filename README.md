## rtc-framework

### 简介
rtc-framework是基于 [Google WebRTC](https://webrtc.org/) 开发的跨平台的实时音视频通信的项目，包括三个子工程：
* signaling-server是WebRTC建立P2P连接的信令服务器，以及管理房间的业务服务器
* vtsrtc是基于WebRTC封装的实时音视频通信库，包括：
   - 视频设备查询
   - 增加视频源
   - 房间管理
   - 实时音视频通信
   - 实时数据通信
* p2p_imgui是使用vtsrtc + Dear ImGui开发的DEMO

### 构建与编译
工程使用xmake组织管理，目前支持：Windows和Linux平台。
* Windows平台推荐使用**VS2019**，VS2017的Debug版本存在问题，种种原因暂时无法解决  
* Linux平台gcc版本需要支持C++14标准（WebRTC库编译要求）
* 为支持ABI，vtsrtc库是C库
#### 安装依赖
* Dear ImGui v1.92.6（由xmake自动拉取）
* OpenGL/X11运行环境（Linux桌面环境通常已具备）
* standalone Asio（signaling-server和vtsrtc网络通信需要）
```
sudo apt-get install build-essential
sudo apt-get install libasio-dev
```

#### 拷贝代码与更新子模块
```
# 克隆仓库
    git clone git@github.com:kunkundi/rtc-framework.git
    cd rtc-framework

# 初始化基础子模块（Simple-Web-Server、Simple-WebSocket-Server）
    git submodule update --init
```

#### 下载第三方依赖（webrtc + Video_Codec_SDK）
由于 webrtc 预编译库和 Video_Codec_SDK 体积较大（~2.7 GB），
采用 **GitHub Releases** 方式管理，不再作为 git 子模块。

**首次克隆后执行以下命令：**

**Linux / macOS:**
```bash
bash scripts/download_third_party.sh
```

**Windows (双击运行或命令行):**
```bat
scripts\download_third_party.bat
```

> 脚本会自动从 GitHub Release 下载并解压到 `third_party/` 目录。
>
> **更新依赖版本**：修改脚本中的 `RELEASE_URL` 版本号即可。
>
> **上传新依赖包到 Release**：
> ```bash
> cd third_party/webrtc
> for d in webrtc-linux webrtc-jetson webrtc-jetson-default webrtc-win; do
>     tar -czf "scripts/packages/${d}.tar.gz" "$d/"
> done
> cd ../Video_Codec_SDK_11.0.10
> tar -czf scripts/packages/Video_Codec_SDK_11.0.10.tar.gz .
> ```
> 然后在 GitHub → Releases → 创建新 Release 并上传 `scripts/packages/` 下的压缩包。

#### Windows平台
1. 运行 `run_xmake.bat`，会分别构建Debug/Release并安装到 `install` 目录
2. 首次构建会自动下载并编译 Dear ImGui v1.92.6 依赖
3. 习惯使用IDE开发，可执行 `xmake project -k vsxmake` 生成Visual Studio工程

#### Linux平台
1. 通用Linux环境执行 `./run_xmake.sh`
2. Jetson(aarch64)环境执行 `./run_xmake_aarch64.sh`
3. 构建产物默认输出到 `build/runtime`，安装输出到 `install`
4. 示例程序为 `build/runtime/p2p_imgui`
5. 习惯使用IDE开发，可执行 `xmake project -k compile_commands` 生成 `compile_commands.json`

#### 常用xmake命令
参照[xmake官方文档](https://xmake.io/)
```
Configure
    xmake f [options]

Build
    xmake [target]

Install
    xmake install -o <install_dir>

Generate compile_commands.json
    xmake project -k compile_commands
```

### 配置和运行
目前只需要修改一处，根据信令服务器的IP地址对应修改 rtc.cfg 文件的api_server和signaling_server字段
