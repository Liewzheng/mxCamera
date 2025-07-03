#!/bin/bash

# mxCamera 集成编译脚本
# 编译所有 lib 开头的子模块并统一输出到 mxCamera/build/lib

# ============================================================================
# 系统兼容性检查
# ============================================================================

# 检查操作系统类型
check_system_compatibility() {
    local os_type=$(uname -s)
    local arch_type=$(uname -m)
    
    # 检查操作系统是否为 Linux
    if [ "$os_type" != "Linux" ]; then
        echo "错误: 此脚本仅支持 Linux 系统运行"
        echo "当前系统: $os_type"
        echo "支持系统: Linux"
        exit 1
    fi
    
    # 检查架构是否为 x86_64
    if [ "$arch_type" != "x86_64" ]; then
        echo "错误: 此脚本仅支持 x86_64 架构运行"
        echo "当前架构: $arch_type"
        echo "支持架构: x86_64"
        echo ""
        echo "说明: 本脚本用于在 x86_64 Linux 主机上交叉编译 ARM 目标程序"
        exit 1
    fi
    
    echo "系统兼容性检查通过: $os_type $arch_type"
}

# 执行系统兼容性检查
check_system_compatibility

echo "=== mxCamera 集成编译脚本 ==="

# 获取项目根目录绝对路径
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# 脚本参数处理
BUILD_TYPE="Release"
CLEAN=false
VERBOSE=false
JOBS=$(nproc)

# 解析命令行参数
while [[ $# -gt 0 ]]; do
    case $1 in
        -d|--debug)
            BUILD_TYPE="Debug"
            shift
            ;;
        -c|--clean)
            CLEAN=true
            shift
            ;;
        -v|--verbose)
            VERBOSE=true
            shift
            ;;
        -j|--jobs)
            JOBS="$2"
            shift 2
            ;;
        -h|--help)
            echo "用法: $0 [选项]"
            echo "选项:"
            echo "  -d, --debug     编译 Debug 版本"
            echo "  -c, --clean     清理后重新编译"
            echo "  -v, --verbose   显示详细编译信息"
            echo "  -j, --jobs N    使用 N 个并行作业"
            echo "  -h, --help      显示此帮助信息"
            exit 0
            ;;
        *)
            echo "未知选项: $1"
            echo "使用 -h 或 --help 查看帮助"
            exit 1
            ;;
    esac
done

# 设置颜色输出
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# 打印彩色消息
print_status() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

print_success() {
    echo -e "${GREEN}[SUCCESS]${NC} $1"
}

print_warning() {
    echo -e "${YELLOW}[WARNING]${NC} $1"
}

print_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# 检查必要工具
check_tools() {
    print_status "检查编译工具..."
    
    # 检查 CMake
    if ! command -v cmake &> /dev/null; then
        print_error "CMake 未安装，请先安装 CMake"
        exit 1
    fi
    CMAKE_VERSION=$(cmake --version | head -n1 | cut -d' ' -f3)
    print_status "CMake 版本: $CMAKE_VERSION"
    
    # 检查 Make
    if ! command -v make &> /dev/null; then
        print_error "Make 未安装，请先安装 Make"
        exit 1
    fi
    MAKE_VERSION=$(make --version | head -n1)
    print_status "Make: $MAKE_VERSION"
}

# 检查本地工具链
check_toolchain() {
    TOOLCHAIN_PREFIX="$PROJECT_ROOT/toolchains/bin/arm-rockchip830-linux-uclibcgnueabihf-"
    
    if [ ! -f "${TOOLCHAIN_PREFIX}gcc" ]; then
        print_error "找不到本地交叉编译工具链"
        print_error "请检查路径: ${TOOLCHAIN_PREFIX}gcc"
        print_error "确保 toolchains/ 目录已正确设置"
        exit 1
    fi
    
    TOOLCHAIN_VERSION=$(${TOOLCHAIN_PREFIX}gcc --version | head -n1)
    print_status "工具链: $TOOLCHAIN_VERSION"
    print_status "工具链路径: $PROJECT_ROOT/toolchains/"
}

# 检查并初始化子模块
check_and_init_submodules() {
    print_status "检查并初始化 Git 子模块..."
    
    # 检查是否在 git 仓库中
    if [ ! -d ".git" ]; then
        print_warning "当前目录不是 Git 仓库，跳过子模块检查"
        return
    fi
    
    # 检查 .gitmodules 文件
    if [ ! -f ".gitmodules" ]; then
        print_warning ".gitmodules 文件不存在，跳过子模块初始化"
        return
    fi
    
    # 初始化子模块
    print_status "初始化 Git 子模块..."
    if git submodule init; then
        print_success "子模块初始化成功"
    else
        print_error "子模块初始化失败"
        exit 1
    fi
    
    # 更新子模块
    print_status "更新 Git 子模块..."
    if git submodule update --recursive; then
        print_success "子模块更新成功"
    else
        print_error "子模块更新失败"
        exit 1
    fi
}

# 准备 LVGL 源码
prepare_lvgl() {
    print_status "准备 LVGL 源码..."
    
    LIBLVGL_DIR="$PROJECT_ROOT/liblvgl"
    
    # 检查 liblvgl 目录是否存在
    if [ ! -d "$LIBLVGL_DIR" ]; then
        print_error "liblvgl 目录不存在: $LIBLVGL_DIR"
        exit 1
    fi
    
    # 检查 fetch_lvgl.sh 脚本是否存在
    if [ ! -f "$LIBLVGL_DIR/fetch_lvgl.sh" ]; then
        print_error "fetch_lvgl.sh 脚本不存在: $LIBLVGL_DIR/fetch_lvgl.sh"
        exit 1
    fi
    
    # 检查 lvgl 目录是否已存在
    if [ -d "$LIBLVGL_DIR/lvgl" ] && [ -d "$LIBLVGL_DIR/lv_drivers" ]; then
        print_status "LVGL 源码已存在，跳过下载"
        return
    fi
    
    # 进入 liblvgl 目录并执行 fetch_lvgl.sh
    print_status "执行 fetch_lvgl.sh 脚本..."
    cd "$LIBLVGL_DIR"
    
    # 确保脚本有执行权限
    chmod +x fetch_lvgl.sh
    
    # 执行获取脚本
    if ./fetch_lvgl.sh; then
        print_success "LVGL 源码获取成功"
    else
        print_error "LVGL 源码获取失败"
        cd "$PROJECT_ROOT"
        exit 1
    fi
    
    # 返回项目根目录
    cd "$PROJECT_ROOT"
    
    # 验证 LVGL 源码是否正确获取
    if [ -d "$LIBLVGL_DIR/lvgl" ] && [ -d "$LIBLVGL_DIR/lv_drivers" ]; then
        print_success "LVGL 源码验证成功"
    else
        print_error "LVGL 源码验证失败"
        exit 1
    fi
}

# 检查子模块
check_submodules() {
    print_status "检查子模块..."
    
    LIB_SUBMODULES=("libgpio" "libmedia" "liblvgl" "libstaging")
    
    for SUBMODULE in "${LIB_SUBMODULES[@]}"; do
        SUBMODULE_DIR="$PROJECT_ROOT/$SUBMODULE"
        if [ -d "$SUBMODULE_DIR" ] && [ -f "$SUBMODULE_DIR/CMakeLists.txt" ]; then
            print_status "$SUBMODULE: OK"
        else
            print_warning "$SUBMODULE: 目录或 CMakeLists.txt 不存在"
        fi
    done
}

# 清理编译目录
clean_build() {
    if [ "$CLEAN" = true ] || [ ! -d "build" ]; then
        print_status "清理编译目录..."
        rm -rf build
        mkdir -p build
        mkdir -p build/lib
        mkdir -p build/bin
    fi
}

# 编译子模块
build_submodules() {
    print_status "开始编译所有子模块..."
    
    LIB_SUBMODULES=("libgpio" "libmedia" "liblvgl" "libstaging")
    TOOLCHAIN_PREFIX_PATH="$PROJECT_ROOT/toolchains"
    
    for SUBMODULE in "${LIB_SUBMODULES[@]}"; do
        SUBMODULE_DIR="$PROJECT_ROOT/$SUBMODULE"
        SUBMODULE_BUILD_DIR="$PROJECT_ROOT/build/$SUBMODULE"
        
        if [ ! -d "$SUBMODULE_DIR" ] || [ ! -f "$SUBMODULE_DIR/CMakeLists.txt" ]; then
            print_warning "跳过 $SUBMODULE: 目录或 CMakeLists.txt 不存在"
            continue
        fi
        
        print_status "编译子模块: $SUBMODULE"
        
        # 清理并创建子模块构建目录
        rm -rf "$SUBMODULE_BUILD_DIR"
        mkdir -p "$SUBMODULE_BUILD_DIR"
        
        # 进入子模块构建目录
        cd "$SUBMODULE_BUILD_DIR"
        
        # 配置子模块CMake，传递工具链参数
        CMAKE_SUBMODULE_ARGS=(
            -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
            -DCMAKE_C_COMPILER="$TOOLCHAIN_PREFIX_PATH/bin/arm-rockchip830-linux-uclibcgnueabihf-gcc"
            -DCMAKE_CXX_COMPILER="$TOOLCHAIN_PREFIX_PATH/bin/arm-rockchip830-linux-uclibcgnueabihf-g++"
            -DCMAKE_AR="$TOOLCHAIN_PREFIX_PATH/bin/arm-rockchip830-linux-uclibcgnueabihf-ar"
            -DCMAKE_STRIP="$TOOLCHAIN_PREFIX_PATH/bin/arm-rockchip830-linux-uclibcgnueabihf-strip"
            -DCMAKE_NM="$TOOLCHAIN_PREFIX_PATH/bin/arm-rockchip830-linux-uclibcgnueabihf-nm"
            -DCMAKE_OBJCOPY="$TOOLCHAIN_PREFIX_PATH/bin/arm-rockchip830-linux-uclibcgnueabihf-objcopy"
            -DCMAKE_OBJDUMP="$TOOLCHAIN_PREFIX_PATH/bin/arm-rockchip830-linux-uclibcgnueabihf-objdump"
            -DCMAKE_SYSTEM_NAME=Linux
            -DCMAKE_SYSTEM_PROCESSOR=arm
            -DTOOLCHAIN_PREFIX="$TOOLCHAIN_PREFIX_PATH"
            -DCMAKE_LIBRARY_OUTPUT_DIRECTORY="$PROJECT_ROOT/build/lib"
            -DCMAKE_LIBRARY_OUTPUT_DIRECTORY_FOR_SUBMODULES="$PROJECT_ROOT/build/lib"
            "$SUBMODULE_DIR"
        )
        
        if [ "$VERBOSE" = true ]; then
            CMAKE_SUBMODULE_ARGS+=(-DCMAKE_VERBOSE_MAKEFILE=ON)
        fi
        
        # 配置子模块
        if cmake "${CMAKE_SUBMODULE_ARGS[@]}"; then
            print_status "$SUBMODULE: CMake 配置成功"
        else
            print_error "$SUBMODULE: CMake 配置失败"
            cd "$PROJECT_ROOT"
            exit 1
        fi
        
        # 编译子模块
        MAKE_SUBMODULE_ARGS=(-j "$JOBS")
        if [ "$VERBOSE" = true ]; then
            MAKE_SUBMODULE_ARGS+=(VERBOSE=1)
        fi
        
        if make "${MAKE_SUBMODULE_ARGS[@]}"; then
            print_success "$SUBMODULE: 编译成功"
        else
            print_error "$SUBMODULE: 编译失败"
            cd "$PROJECT_ROOT"
            exit 1
        fi
        
        # 返回项目根目录
        cd "$PROJECT_ROOT"
    done
    
    print_success "所有子模块编译完成"
    
    # 显示编译结果
    print_status "子模块库文件:"
    if [ -d "build/lib" ]; then
        ls -la build/lib/
    fi
}

# 配置 CMake
configure_cmake() {
    print_status "配置 CMake (构建类型: $BUILD_TYPE)..."
    
    cd build
    
    CMAKE_ARGS=(
        -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
        ..
    )
    
    if [ "$VERBOSE" = true ]; then
        CMAKE_ARGS+=(-DCMAKE_VERBOSE_MAKEFILE=ON)
    fi
    
    if cmake "${CMAKE_ARGS[@]}"; then
        print_success "CMake 配置成功"
    else
        print_error "CMake 配置失败"
        exit 1
    fi
    
    cd ..
}

# 编译项目
build_project() {
    print_status "开始编译 (使用 $JOBS 个并行作业)..."
    
    cd build
    
    MAKE_ARGS=(-j "$JOBS")
    
    if [ "$VERBOSE" = true ]; then
        MAKE_ARGS+=(VERBOSE=1)
    fi
    
    if make "${MAKE_ARGS[@]}"; then
        print_success "编译成功!"
    else
        print_error "编译失败!"
        exit 1
    fi
    
    cd ..
}

# 显示编译结果
show_results() {
    cd "$PROJECT_ROOT"
    
    echo ""
    echo "=== 编译输出信息 ==="
    
    # 显示库文件
    print_status "已编译的库文件:"
    if [ -d "build/lib" ]; then
        ls -la build/lib/
    else
        print_warning "build/lib 目录不存在"
    fi
    
    # 显示可执行文件
    if [ -f "build/bin/mxCamera" ]; then
        print_success "可执行文件生成成功!"
        echo ""
        echo "可执行文件:"
        ls -la build/bin/mxCamera
        file build/bin/mxCamera
        
        # 显示二进制文件信息
        TOOLCHAIN_PREFIX="$PROJECT_ROOT/toolchains/bin/arm-rockchip830-linux-uclibcgnueabihf-"
        if [ -f "${TOOLCHAIN_PREFIX}readelf" ]; then
            echo ""
            echo "目标架构: $(${TOOLCHAIN_PREFIX}readelf -h build/bin/mxCamera | grep Machine)"
            
            # 显示动态库依赖
            echo ""
            echo "动态库依赖:"
            ${TOOLCHAIN_PREFIX}readelf -d build/bin/mxCamera | grep NEEDED || echo "  无外部动态库依赖"
        fi
        
        echo ""
        echo "=== 部署说明 ==="
        echo "1. 将文件拷贝到 Luckfox Pico 设备:"
        echo "   scp build/bin/mxCamera root@<target_ip>:~/"
        echo "   scp build/lib/*.so* root@<target_ip>:/usr/lib/"
        echo ""
        echo "2. 在设备上运行:"
        echo "   export LD_LIBRARY_PATH=/usr/lib:\$LD_LIBRARY_PATH"
        echo "   chmod +x mxCamera"
        echo "   ./mxCamera"
        
    else
        print_error "未找到可执行文件 build/bin/mxCamera"
        print_status "检查编译日志以确定问题"
        exit 1
    fi
}

# 创建部署包
create_deployment_package() {
    print_status "创建部署包..."
    
    # 检查zip命令是否存在
    if ! command -v zip &> /dev/null; then
        print_error "zip 命令未找到，请安装 zip 工具"
        print_error "Ubuntu/Debian: sudo apt install zip"
        print_error "CentOS/RHEL: sudo yum install zip"
        exit 1
    fi
    
    # 创建临时目录用于打包
    PACKAGE_DIR="$PROJECT_ROOT/build/package"
    rm -rf "$PACKAGE_DIR"
    mkdir -p "$PACKAGE_DIR"
    
    # 复制二进制文件
    print_status "复制二进制文件..."
    if [ -d "build/bin" ]; then
        cp -r build/bin "$PACKAGE_DIR/"
    else
        print_error "build/bin 目录不存在"
        exit 1
    fi
    
    # 复制库文件（去掉符号链接，只保留实际文件）
    print_status "复制库文件（去掉符号链接）..."
    if [ -d "build/lib" ]; then
        mkdir -p "$PACKAGE_DIR/lib"
        # 只复制实际的.so文件，跳过符号链接
        find build/lib -name "*.so*" -type f -exec cp {} "$PACKAGE_DIR/lib/" \;
    else
        print_error "build/lib 目录不存在"
        exit 1
    fi
    
    # 复制部署脚本
    print_status "复制部署脚本..."
    if [ -f "deploy.ps1" ]; then
        cp deploy.ps1 "$PACKAGE_DIR/"
    else
        print_warning "deploy.ps1 不存在，跳过"
    fi
    
    # 复制启动脚本
    if [ -f "mxcamera" ]; then
        cp mxcamera "$PACKAGE_DIR/"
    else
        print_warning "mxcamera 启动脚本不存在，跳过"
    fi
    
    # 创建部署说明文件
    cat > "$PACKAGE_DIR/README.txt" << EOF
mxCamera 部署包
==============

此包包含：
- bin/mxCamera: 主程序可执行文件
- lib/*.so.*: 动态库文件
- deploy.ps1: Windows PowerShell 部署脚本
- mxcamera: Linux 启动服务脚本
- README.txt: 本说明文件

部署方法：
=========

方法一：使用 PowerShell 自动部署（推荐）
1. 确保设备已通过 ADB 连接
2. 在 Windows 上以管理员身份运行 PowerShell
3. 执行: .\deploy.ps1

方法二：手动部署
1. 将 bin/mxCamera 复制到设备的 /root/Workspace/
2. 将 lib/*.so.* 复制到设备的 /usr/lib/
3. 将 mxcamera 复制到设备的 /etc/init.d/S99mxcamera
4. 在设备上执行: chmod +x /root/Workspace/mxCamera
5. 在设备上执行: chmod +x /etc/init.d/S99mxcamera

注意事项：
=========
- 目标架构: ARM (Luckfox Pico)
- 需要 root 权限进行部署
- 建议先停止冲突的服务

生成时间: $(date '+%Y-%m-%d %H:%M:%S')
构建类型: $BUILD_TYPE
EOF
    
    # 生成部署包文件名
    local timestamp=$(date '+%Y%m%d_%H%M%S')
    local package_name="mxCamera_${BUILD_TYPE}_${timestamp}.zip"
    local package_path="$PROJECT_ROOT/$package_name"
    
    # 创建 zip 包
    print_status "创建 ZIP 包: $package_name"
    cd "$PACKAGE_DIR"
    if zip -r "$package_path" . > /dev/null 2>&1; then
        print_success "部署包创建成功: $package_name"
    else
        print_error "部署包创建失败"
        cd "$PROJECT_ROOT"
        exit 1
    fi
    
    cd "$PROJECT_ROOT"
    
    # 显示包内容
    print_status "部署包内容:"
    unzip -l "$package_path" | grep -v "Archive:" | grep -v "Length" | grep -v "^$" | head -20
    
    # 显示包信息
    local package_size=$(ls -lh "$package_path" | awk '{print $5}')
    echo ""
    print_success "=== 部署包信息 ==="
    echo "文件名: $package_name"
    echo "大小: $package_size"
    echo "路径: $package_path"
    echo ""
    print_status "=== Windows 部署说明 ==="
    echo "1. 将 $package_name 下载到 Windows 计算机"
    echo "2. 解压缩到任意目录"
    echo "3. 确保 Luckfox Pico 设备已通过 ADB 连接"
    echo "4. 以管理员身份运行 PowerShell"
    echo "5. 进入解压目录，执行: .\\deploy.ps1"
    
    # 清理临时目录
    rm -rf "$PACKAGE_DIR"
}

# 主执行流程
main() {
    # 确保在项目根目录执行
    cd "$PROJECT_ROOT"
    
    echo "构建类型: $BUILD_TYPE"
    echo "并行作业: $JOBS"
    echo "详细输出: $VERBOSE"
    echo "清理重建: $CLEAN"
    echo ""
    
    check_tools
    check_toolchain
    check_and_init_submodules
    prepare_lvgl
    check_submodules
    
    clean_build
    build_submodules    # 先编译所有子模块
    configure_cmake     # 再配置主项目
    build_project       # 最后编译主项目
    show_results        # 显示编译结果
    create_deployment_package  # 创建部署包
    
    print_success "集成编译脚本执行完成!"
}

# 执行主函数
main "$@"
