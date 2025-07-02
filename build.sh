#!/bin/bash

# mxCamera 项目编译脚本
# 集成 libgpio、libMedia 和 LVGL 的摄像头实时显示系统

echo "=== mxCamera 项目编译脚本 ==="

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

# 检查工具链
check_toolchain() {
    TOOLCHAIN_PREFIX="/home/liewzheng/Workspace/luckfox-pico/tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf/bin/arm-rockchip830-linux-uclibcgnueabihf-"
    
    if [ ! -f "${TOOLCHAIN_PREFIX}gcc" ]; then
        print_error "找不到交叉编译工具链"
        print_error "请检查路径: ${TOOLCHAIN_PREFIX}gcc"
        exit 1
    fi
    
    TOOLCHAIN_VERSION=$(${TOOLCHAIN_PREFIX}gcc --version | head -n1)
    print_status "工具链: $TOOLCHAIN_VERSION"
}

# 检查依赖库
check_dependencies() {
    print_status "检查依赖库..."
    
    # 检查 libgpio
    LIBGPIO_DIR="$PROJECT_ROOT/../libgpio"
    if [ ! -f "$LIBGPIO_DIR/build/libgpio.so" ]; then
        print_error "libgpio 库未找到，请先编译 libgpio"
        print_status "进入 $LIBGPIO_DIR 目录执行 ./build.sh"
        exit 1
    fi
    print_status "libgpio: OK"
    
    # 检查 libMedia
    LIBMEDIA_DIR="$PROJECT_ROOT/../libMedia"
    if [ ! -f "$LIBMEDIA_DIR/build/libmedia.so" ]; then
        print_error "libMedia 库未找到，请先编译 libMedia"
        print_status "进入 $LIBMEDIA_DIR 目录执行 ./build.sh"
        exit 1
    fi
    print_status "libMedia: OK"
    
    # 检查 LVGL
    LVGL_DIR="$PROJECT_ROOT/../../lvgl/build"
    if [ ! -f "$LVGL_DIR/liblvgl.so" ]; then
        print_error "LVGL 库未找到，请先编译 LVGL"
        print_status "进入 lvgl_demo 目录执行 ./build.sh"
        exit 1
    fi
    print_status "LVGL: OK"
}

# 清理编译目录
clean_build() {
    if [ "$CLEAN" = true ] || [ ! -d "build" ]; then
        print_status "清理编译目录..."
        rm -rf build
        mkdir -p build
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
    
    if [ -f "build/bin/mxCamera" ]; then
        print_success "可执行文件生成成功!"
        echo ""
        echo "=== 编译输出信息 ==="
        
        # 显示可执行文件信息
        echo "可执行文件:"
        ls -la build/bin/mxCamera
        file build/bin/mxCamera
        
        # 显示依赖库信息
        echo ""
        echo "依赖库:"
        echo "  libgpio: $(ls -la ../libgpio/build/libgpio.so*)"
        echo "  libMedia: $(ls -la ../libMedia/build/libmedia.so*)"
        echo "  LVGL: $(ls -la ../../lvgl/build/liblvgl.so*)"
        
        # 显示二进制文件信息
        TOOLCHAIN_PREFIX="/home/liewzheng/Workspace/luckfox-pico/tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf/bin/arm-rockchip830-linux-uclibcgnueabihf-"
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
        echo "   scp ../libgpio/build/libgpio.so root@<target_ip>:/usr/lib/"
        echo "   scp ../libMedia/build/libmedia.so root@<target_ip>:/usr/lib/"
        echo "   scp ../../lvgl/build/liblvgl.so root@<target_ip>:/usr/lib/"
        echo ""
        echo "2. 在设备上运行:"
        echo "   export LD_LIBRARY_PATH=/usr/lib:\$LD_LIBRARY_PATH"
        echo "   chmod +x mxCamera"
        echo "   ./mxCamera"
        echo ""
        echo "=== 功能说明 ==="
        echo "- 实时显示摄像头采集的图像"
        echo "- 右上角显示当前帧率"
        echo "- 左上角显示运行状态"
        echo "- KEY0 按键控制摄像头暂停/恢复"
        echo "- Ctrl+C 退出程序"
        
    else
        print_error "未找到可执行文件 build/bin/mxCamera"
        exit 1
    fi
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
    check_dependencies
    
    clean_build
    configure_cmake
    build_project
    show_results
    
    print_success "编译脚本执行完成!"
}

# 执行主函数
main "$@"
