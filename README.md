## rtc-solutions

### 简介
rtc-solutions是基于 [Google WebRTC](https://webrtc.org/) 开发的跨平台的实时音视频通信的项目，包括三个子工程：
* signaling-server是WebRTC建立P2P连接的信令服务器，以及管理房间的业务服务器
* vtsrtc是基于WebRTC封装的实时音视频通信库，包括：
   - 视频设备查询
   - 增加视频源
   - 房间管理
   - 实时音视频通信
   - 实时数据通信
* p2p_qt是使用vtsrtc开发的DEMO

### 构建与编译
工程使用CMake组织管理，目前支持：Windows和Linux平台。
* Windows平台推荐使用**VS2019**，VS2017的Debug版本存在问题，种种原因暂时无法解决  
* Linux平台gcc版本需要支持C++14标准（WebRTC库编译要求）
* 为支持ABI，vtsrtc库是C库
#### 安装依赖
* QT（运行p2p_qt DEMO需要）
* Boost（signaling-server和vtsrtc网络通信需要）
* 在share目录 \\\10.11.16.35\share\个人\朱建\rtc-solutions 下，准备了QT和Boost
* Ubuntu（aarch64验证过，x86_64没有验证过，估计也可以）下安装QT
```
sudo apt-get install build-essential
sudo apt-get install qtcreator
sudo apt-get install qt5-default
```

#### 拷贝代码与更新子模块
```
Clone代码
    git clone ssh://git@10.11.16.35:2022/zhujian/rtc-solutions.git
切换到v0.2.0 tag
    git checkout v0.2.0
更新子模块
    git submodule update --init
```

#### Windows平台
1. 打开根目录下的 run_cmake_msvcxxx.bat 脚本，对应修改QTDIR和BOOST_ROOT的值
2. 运行 run_cmake_msvcxxx.bat 脚本，将会进行工程编译和类库安装
   - build_msvcxxx/runtime目录下保存Debug和Release的signaling-server和p2p_qt可执行程序，测试数据和依赖dll会自动拷贝好
   - install_msvcxxx目录下保存Debug和Release的vtsrtc类库
3. 习惯使用Visual Studio IDE，可以直接打开build_msvcxxx/rtc-solutions.sln解决方案，调试路径已经自动设置好

#### Linux平台
1. 打开根目录下的 run_cmake.sh 脚本，对应修改QTDIR的值
2. 运行 run_cmake.sh 脚本，将会进行工程编译和类库安装
   - build/runtime目录下保存Release的signaling-server和p2p_qt可执行程序
   - install目录下保存Release的vtsrtc类库
3. 习惯使用IDE开发，可以使用QT Creator或者CLion打开CMake工程

#### 常用CMake命令
参照[CMake官方文档](https://cmake.org/cmake/help/latest/manual/cmake.1.html#manual:cmake(1))
```
Build a Project
    cmake --build <dir> [<options>] [-- <build-tool-options>]

Install a Project
    cmake --install <dir> [<options>]
```

### 配置和运行
目前只需要修改一处，根据信令服务器的IP地址对应修改 rtc.cfg 文件的api_server和signaling_server字段
