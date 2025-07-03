# mxCamera 开发者手册

一个基于 Luckfox Pico 平台的实时相机显示系统，支持自动化编译与部署。

## 📋 目录

- [项目概述](#项目概述)
- [开发环境要求](#开发环境要求)
- [快速开始](#快速开始)
- [项目结构](#项目结构)
- [编译系统](#编译系统)
- [部署方式](#部署方式)
- [开发指南](#开发指南)
- [故障排除](#故障排除)
- [常见问题](#常见问题)

## 🎯 项目概述

mxCamera 是一个为 Luckfox Pico (RV1103) 平台开发的实时相机显示系统，具有以下特点：

- **模块化设计**：分离的库模块（GPIO、媒体、LVGL、Staging）
- **自动化编译**：一键编译所有依赖和主程序
- **跨平台部署**：支持 Windows 用户通过 ADB 快速部署
- **交叉编译**：在 x86_64 Linux 主机上编译 ARM 目标程序

## 💻 开发环境要求

### 支持的主机平台

| 平台 | 架构 | 状态 |
|------|------|------|
| Linux | x86_64 | ✅ 支持 |
| Linux | ARM64 | ❌ 不支持 |
| Windows | x86_64 | ❌ 不支持编译 |
| macOS | x86_64/ARM64 | ❌ 不支持编译 |

> **注意**：编译环境仅支持 Linux x86_64，其他平台可以使用预编译的部署包。

### 必需工具

#### Linux 开发环境

```bash
# Ubuntu/Debian 系统
sudo apt update
sudo apt install -y \
    build-essential \
    cmake \
    git \
    zip \
    unzip

# CentOS/RHEL 系统
sudo yum groupinstall -y "Development Tools"
sudo yum install -y cmake git zip unzip

# Arch Linux
sudo pacman -S base-devel cmake git zip unzip
```

#### Windows 部署环境

```powershell
# 安装 Android SDK Platform Tools (包含 ADB)
# 下载地址: https://developer.android.com/studio/releases/platform-tools
# 解压后将 platform-tools 目录添加到 PATH 环境变量
```

### 工具链

项目使用内置的 ARM 交叉编译工具链：
- **位置**：`toolchains/bin/`
- **前缀**：`arm-rockchip830-linux-uclibcgnueabihf-`
- **GCC 版本**：8.3.0
- **目标平台**：ARM EABI5 (Luckfox Pico RV1103)

## 🚀 快速开始

### 1. 克隆项目

```bash
git clone <repository-url>
cd luckfox_pico_app/mxCamera
```

### 2. 初始化子模块

```bash
git submodule update --init --recursive
```

### 3. 一键编译

```bash
# 赋予执行权限
chmod +x build.sh

# 执行完整编译（推荐）
./build.sh --clean

# 或者增量编译
./build.sh
```

### 4. 获取部署包

编译完成后，会在项目根目录生成：
```
mxCamera_Release_YYYYMMDD_HHMMSS.zip
```

### 5. 部署到设备

**方式一：Windows 自动部署**
1. 将 zip 文件发送给 Windows 用户
2. 解压到任意目录
3. 以管理员身份运行 PowerShell
4. 执行：`.\deploy.ps1`

**方式二：手动部署**
```bash
# 复制程序文件
scp build/bin/mxCamera root@<device-ip>:/root/Workspace/
scp build/lib/*.so.* root@<device-ip>:/usr/lib/
scp mxcamera root@<device-ip>:/etc/init.d/S99mxcamera

# 设置权限
ssh root@<device-ip> "chmod +x /root/Workspace/mxCamera"
ssh root@<device-ip> "chmod +x /etc/init.d/S99mxcamera"
```

## 📁 项目结构

```
mxCamera/
├── build.sh                    # 🔧 主编译脚本
├── CMakeLists.txt              # 📋 主项目 CMake 配置
├── deploy.ps1                  # 🚀 Windows 部署脚本
├── mxcamera                    # 🎯 Linux 启动服务脚本
├── README.md                   # 📖 本文档
│
├── toolchains/                 # 🛠️ ARM 交叉编译工具链
│   └── bin/
│       ├── arm-rockchip830-linux-uclibcgnueabihf-gcc
│       ├── arm-rockchip830-linux-uclibcgnueabihf-g++
│       └── ...
│
├── include/                    # 📂 主项目头文件
│   ├── Debug.h
│   ├── DEV_Config.h
│   ├── lv_conf.h
│   └── lv_drv_conf.h
│
├── source/                     # 📂 主项目源代码
│   ├── DEV_Config.c
│   └── main.c
│
├── cmake/                      # ⚙️ CMake 工具
│   └── toolchain-arm-linux.cmake
│
└── 子模块/                     # 📦 库模块
    ├── libgpio/               # GPIO 操作库
    ├── libmedia/              # 媒体处理库
    ├── liblvgl/               # LVGL 图形库
    └── libstaging/            # 临时功能库
```

## 🔨 编译系统

### build.sh 脚本选项

```bash
./build.sh [选项]

选项：
  -d, --debug     编译 Debug 版本（默认：Release）
  -c, --clean     清理后重新编译
  -v, --verbose   显示详细编译信息
  -j, --jobs N    使用 N 个并行作业（默认：CPU 核心数）
  -h, --help      显示帮助信息
```

### 编译流程

1. **系统兼容性检查**
   - 验证 Linux x86_64 环境
   - 检查必需工具（cmake, make, gcc）

2. **工具链验证**
   - 检查 ARM 交叉编译工具链
   - 验证工具链版本和路径

3. **子模块处理**
   - 初始化和更新 Git 子模块
   - 准备 LVGL 源码（自动下载）

4. **模块编译**
   - 独立编译每个库模块
   - 统一输出到 `build/lib/`

5. **主程序编译**
   - 链接预编译的库文件
   - 生成最终可执行文件

6. **自动打包**
   - 创建部署包 ZIP 文件
   - 生成部署说明文档

### 输出目录结构

```
build/
├── bin/
│   └── mxCamera                # 主程序可执行文件
├── lib/
│   ├── libgpio.so.*           # GPIO 库
│   ├── libmedia.so.*          # 媒体库
│   ├── liblvgl.so.*           # LVGL 库
│   └── libstaging.so.*        # Staging 库
└── package/                   # 临时打包目录
```

## 🚀 部署方式

### Windows PowerShell 部署（推荐）

**前提条件：**
- 安装 Android SDK Platform Tools
- 设备通过 USB 连接并开启 ADB 调试
- 以管理员身份运行 PowerShell

**部署步骤：**
```powershell
# 1. 解压部署包
Expand-Archive mxCamera_Release_*.zip -DestinationPath .\mxCamera

# 2. 进入目录
cd mxCamera

# 3. 执行部署
.\deploy.ps1
```

**部署内容：**
- 主程序 → `/root/Workspace/mxCamera`
- 库文件 → `/usr/lib/lib*.so.*`
- 启动脚本 → `/etc/init.d/S99mxcamera`
- 自动禁用冲突服务

### 手动部署

**通过 SCP：**
```bash
# 设备 IP 地址
DEVICE_IP="192.168.1.100"

# 复制文件
scp build/bin/mxCamera root@$DEVICE_IP:/root/Workspace/
scp build/lib/*.so.* root@$DEVICE_IP:/usr/lib/
scp mxcamera root@$DEVICE_IP:/etc/init.d/S99mxcamera

# 设置权限
ssh root@$DEVICE_IP "chmod +x /root/Workspace/mxCamera"
ssh root@$DEVICE_IP "chmod +x /etc/init.d/S99mxcamera"
```

**通过 ADB：**
```bash
# 推送文件
adb push build/bin/mxCamera /root/Workspace/mxCamera
adb push build/lib/*.so.* /usr/lib/
adb push mxcamera /etc/init.d/S99mxcamera

# 设置权限
adb shell "chmod +x /root/Workspace/mxCamera"
adb shell "chmod +x /etc/init.d/S99mxcamera"
```

## 🛠️ 开发指南

### 添加新的库模块

1. **创建子目录**
   ```bash
   mkdir lib<module_name>
   cd lib<module_name>
   ```

2. **创建 CMakeLists.txt**
   ```cmake
   cmake_minimum_required(VERSION 3.10)
   project(libmodule VERSION 1.0.0)
   
   # 配置交叉编译
   include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/common.cmake)
   
   # 添加源文件
   file(GLOB SOURCES "source/*.c")
   
   # 创建共享库
   add_library(module SHARED ${SOURCES})
   
   # 设置输出目录
   set_target_properties(module PROPERTIES
       LIBRARY_OUTPUT_DIRECTORY "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}"
   )
   ```

3. **更新 build.sh**
   ```bash
   # 在 LIB_SUBMODULES 数组中添加新模块
   LIB_SUBMODULES=("libgpio" "libmedia" "liblvgl" "libstaging" "lib<module_name>")
   ```

4. **更新主项目 CMakeLists.txt**
   ```cmake
   # 查找并链接新库
   find_library(MODULE_LIB module PATHS ${CMAKE_CURRENT_SOURCE_DIR}/build/lib REQUIRED)
   target_link_libraries(mxCamera ${MODULE_LIB})
   ```

### 修改编译配置

**更改编译类型：**
```bash
# Debug 编译
./build.sh --debug

# Release 编译（默认）
./build.sh
```

**修改编译参数：**
编辑 `build.sh` 中的 `CMAKE_SUBMODULE_ARGS` 和 `CMAKE_ARGS` 数组。

**添加编译器标志：**
```cmake
# 在各模块的 CMakeLists.txt 中
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -your-flag")
```

### 调试技巧

**详细编译输出：**
```bash
./build.sh --verbose
```

**单独编译子模块：**
```bash
cd build/libgpio
make VERBOSE=1
```

**检查链接依赖：**
```bash
# 在编译完成后
arm-rockchip830-linux-uclibcgnueabihf-readelf -d build/bin/mxCamera
```

## 🔍 故障排除

### 编译错误

**错误：系统不兼容**
```
错误: 此脚本仅支持 Linux 系统运行
```
**解决：** 仅在 Linux x86_64 系统上编译，其他平台使用预编译包。

**错误：工具链缺失**
```
错误: 找不到本地交叉编译工具链
```
**解决：** 确保 `toolchains/` 目录存在且包含 ARM 工具链。

**错误：CMake 配置失败**
```
CMake Error: CMAKE_C_COMPILER not set
```
**解决：** 检查工具链路径，确保 gcc 可执行。

### 部署错误

**错误：ADB 未找到**
```
ADB not found! Please install Android SDK Platform Tools
```
**解决：** 下载并安装 Android SDK Platform Tools，添加到 PATH。

**错误：设备未连接**
```
未找到连接的设备或设备未授权
```
**解决：** 
1. 检查 USB 连接
2. 确保设备开启 ADB 调试
3. 运行 `adb devices` 确认设备状态

**错误：权限不足**
```
Permission denied
```
**解决：** 以管理员身份运行 PowerShell（Windows）或使用 sudo（Linux）。

### 运行时错误

**错误：库文件缺失**
```
error while loading shared libraries: libxxx.so.1: cannot open shared object file
```
**解决：** 
1. 确保所有 `.so.*` 文件已复制到 `/usr/lib/`
2. 运行 `ldconfig` 更新库缓存
3. 检查 `LD_LIBRARY_PATH` 环境变量

**错误：段错误**
```
Segmentation fault
```
**解决：** 
1. 使用 Debug 版本编译：`./build.sh --debug`
2. 检查设备是否有足够内存
3. 确保所有依赖库版本匹配

## ❓ 常见问题

### Q: 编译需要多长时间？
A: 首次编译约 5-10 分钟（取决于硬件），增量编译通常 1-2 分钟。

### Q: 可以在虚拟机中编译吗？
A: 可以，但建议分配足够的 CPU 核心和内存（至少 4GB）。

### Q: 如何减小编译产物体积？
A: 使用 Release 模式编译，并在链接时添加 `-s` 标志去除调试符号。

### Q: 支持其他 ARM 设备吗？
A: 当前专门为 Luckfox Pico RV1103 优化，移植到其他设备需要修改工具链配置。

### Q: 如何备份编译环境？
A: 整个项目目录是自包含的，直接复制即可迁移。

### Q: Windows 用户可以修改代码吗？
A: 可以修改，但需要在 Linux 环境中重新编译。建议使用 WSL 或 Docker。

---

## 📞 技术支持

遇到问题时，请提供以下信息：
- 主机操作系统和架构
- 编译输出的完整日志
- 错误信息截图
- build.sh 执行参数

**编译日志收集：**
```bash
./build.sh --verbose 2>&1 | tee build.log
```

---

## 📝 版本历史

| 版本 | 日期 | 变更说明 |
|------|------|----------|
| 1.0.0 | 2025-07-03 | 初始版本，支持自动化编译与部署 |

---

*最后更新：2025年7月3日*