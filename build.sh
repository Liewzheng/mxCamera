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

# 检查是否为首次运行
check_first_run() {
    local is_first_run=false
    
    # 检查是否缺少关键目录或文件
    if [ ! -d "build" ] || [ ! -d ".git" ]; then
        is_first_run=true
    fi
    
    # 检查子模块是否为空
    if [ -d ".git" ] && [ -f ".gitmodules" ]; then
        local submodule_paths=($(git config --file .gitmodules --get-regexp path | awk '{ print $2 }'))
        for submodule_path in "${submodule_paths[@]}"; do
            if [ ! -d "$submodule_path" ] || [ -z "$(ls -A "$submodule_path" 2>/dev/null)" ]; then
                is_first_run=true
                break
            fi
        done
    fi
    
    if [ "$is_first_run" = true ]; then
        echo ""
        print_status "=== 检测到首次运行或仓库不完整 ==="
        echo ""
        echo "👋 欢迎使用 mxCamera 项目！"
        echo ""
        echo "本脚本将自动为您完成以下操作："
        echo "  📦 检查并拉取 Git 子模块"
        echo "  📥 下载 LVGL 源码依赖"
        echo "  🔧 配置交叉编译环境"
        echo "  🔨 编译所有库和主程序"
        echo "  📄 生成部署包"
        echo ""
        echo "首次编译可能需要 5-10 分钟，请耐心等待..."
        echo ""
        
        # 询问用户是否继续
        if [ -t 0 ]; then  # 检查是否在交互式终端中
            read -p "是否继续？[Y/n] " -n 1 -r
            echo
            if [[ $REPLY =~ ^[Nn]$ ]]; then
                echo "用户取消操作"
                exit 0
            fi
        fi
        
        echo ""
    fi
}

# 检查首次运行
check_first_run

# 脚本参数处理
BUILD_TYPE="Release"
CLEAN=false
FORCE_CLEAN=false
VERBOSE=false
JOBS=$(nproc)
INCREMENTAL=true

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
        --force-clean)
            FORCE_CLEAN=true
            CLEAN=true
            shift
            ;;
        --full)
            CLEAN=true
            INCREMENTAL=false
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
            echo "  -d, --debug       编译 Debug 版本"
            echo "  -c, --clean       清理后重新编译"
            echo "  --force-clean     强制完全清理（包括子模块）"
            echo "  --full            执行完全编译（等同于 --clean）"
            echo "  -v, --verbose     显示详细编译信息"
            echo "  -j, --jobs N      使用 N 个并行作业"
            echo "  -h, --help        显示此帮助信息"
            echo ""
            echo "增量编译模式:"
            echo "  默认执行增量编译，只重新编译有变化的部分"
            echo "  子模块仅在源码有更新时重新编译"
            echo "  主项目根据依赖关系智能编译"
            exit 0
            ;;
        *)
            echo "未知选项: $1"
            echo "使用 -h 或 --help 查看帮助"
            exit 1
            ;;
    esac
done

# 检查必要工具
check_tools() {
    print_status "检查编译工具..."
    
    local missing_tools=()
    
    # 检查 CMake
    if ! command -v cmake &> /dev/null; then
        print_error "CMake 未安装"
        missing_tools+=("cmake")
    else
        CMAKE_VERSION=$(cmake --version | head -n1 | cut -d' ' -f3)
        print_status "CMake 版本: $CMAKE_VERSION"
    fi
    
    # 检查 Make
    if ! command -v make &> /dev/null; then
        print_error "Make 未安装"
        missing_tools+=("make")
    else
        MAKE_VERSION=$(make --version | head -n1)
        print_status "Make: $MAKE_VERSION"
    fi
    
    # 检查 Git
    if ! command -v git &> /dev/null; then
        print_error "Git 未安装"
        missing_tools+=("git")
    else
        GIT_VERSION=$(git --version)
        print_status "$GIT_VERSION"
    fi
    
    # 检查 zip
    if ! command -v zip &> /dev/null; then
        print_error "zip 未安装"
        missing_tools+=("zip")
    else
        ZIP_VERSION=$(zip -v | head -n2 | tail -n1 | awk '{print $1, $2}')
        print_status "zip 版本: $ZIP_VERSION"
    fi
    
    # 如果有缺失的工具，提供安装指南并退出
    if [ ${#missing_tools[@]} -gt 0 ]; then
        echo ""
        print_error "=== 缺失必要工具 ==="
        echo ""
        echo "以下工具未安装或不在 PATH 中："
        for tool in "${missing_tools[@]}"; do
            echo "  ❌ $tool"
        done
        echo ""
        print_status "=== 安装指南 ==="
        echo ""
        echo "Ubuntu/Debian 系统："
        echo "  sudo apt update"
        echo "  sudo apt install -y cmake make git zip"
        echo ""
        echo "CentOS/RHEL 系统："
        echo "  sudo yum install -y cmake make git zip"
        echo "  # 或使用 dnf (较新版本):"
        echo "  sudo dnf install -y cmake make git zip"
        echo ""
        echo "Arch Linux 系统："
        echo "  sudo pacman -S cmake make git zip"
        echo ""
        echo "Alpine Linux 系统："
        echo "  sudo apk add cmake make git zip"
        echo ""
        echo "Fedora 系统："
        echo "  sudo dnf install -y cmake make git zip"
        echo ""
        print_status "安装完成后，请重新运行此脚本。"
        exit 1
    fi
    
    print_success "所有必要工具检查通过"
}

# 检查本地工具链
check_toolchain() {
    print_status "检查交叉编译工具链..."
    
    TOOLCHAIN_PREFIX="$PROJECT_ROOT/toolchains/bin/arm-rockchip830-linux-uclibcgnueabihf-"
    
    # 检查工具链目录是否存在
    if [ ! -d "$PROJECT_ROOT/toolchains" ]; then
        print_error "工具链目录不存在: $PROJECT_ROOT/toolchains"
        print_error "这通常表示 toolchains 子模块未正确拉取"
        
        # 检查是否在 git 仓库中
        if [ -d ".git" ] && [ -f ".gitmodules" ]; then
            print_status "=== 诊断信息 ==="
            echo "检查子模块状态:"
            git submodule status | grep toolchains || echo "  未找到 toolchains 子模块定义"
            
            print_status "=== 建议的解决方案 ==="
            echo "1. 检查 .gitmodules 文件是否包含 toolchains 子模块"
            echo "2. 手动拉取工具链子模块:"
            echo "   git submodule update --init --recursive toolchains"
            echo "3. 如果问题持续存在，请检查子模块 URL 配置"
        else
            print_status "=== 建议的解决方案 ==="
            echo "1. 确保已克隆完整的项目仓库"
            echo "2. 检查 toolchains/ 目录是否存在于项目根目录"
            echo "3. 如果工具链是单独提供的，请将其解压到 toolchains/ 目录"
        fi
        
        exit 1
    fi
    
    # 检查工具链可执行文件
    if [ ! -f "${TOOLCHAIN_PREFIX}gcc" ]; then
        print_error "找不到交叉编译工具链可执行文件"
        print_error "期望路径: ${TOOLCHAIN_PREFIX}gcc"
        
        # 显示 toolchains 目录内容以便诊断
        print_status "=== toolchains 目录内容 ==="
        if [ -d "$PROJECT_ROOT/toolchains/bin" ]; then
            echo "bin/ 目录内容:"
            ls -la "$PROJECT_ROOT/toolchains/bin/" | head -10
        else
            echo "bin/ 目录不存在"
            echo "toolchains/ 目录内容:"
            ls -la "$PROJECT_ROOT/toolchains/" | head -10
        fi
        
        print_status "=== 建议的解决方案 ==="
        echo "1. 检查工具链文件是否正确解压"
        echo "2. 确保工具链文件有执行权限:"
        echo "   chmod +x $PROJECT_ROOT/toolchains/bin/*"
        echo "3. 检查工具链文件名是否正确"
        echo "4. 如果使用的是不同的工具链，请更新脚本中的工具链路径"
        
        exit 1
    fi
    
    # 检查工具链文件权限
    if [ ! -x "${TOOLCHAIN_PREFIX}gcc" ]; then
        print_warning "工具链文件没有执行权限，正在设置..."
        chmod +x "$PROJECT_ROOT/toolchains/bin/"* || {
            print_error "无法设置工具链执行权限"
            exit 1
        }
        print_success "工具链执行权限设置完成"
    fi
    
    # 获取并显示工具链信息
    TOOLCHAIN_VERSION=$(${TOOLCHAIN_PREFIX}gcc --version 2>/dev/null | head -n1)
    if [ $? -eq 0 ]; then
        print_success "工具链验证成功"
        print_status "工具链版本: $TOOLCHAIN_VERSION"
        print_status "工具链路径: $PROJECT_ROOT/toolchains/"
        
        # 显示目标架构信息
        TARGET_ARCH=$(${TOOLCHAIN_PREFIX}gcc -dumpmachine 2>/dev/null)
        if [ $? -eq 0 ]; then
            print_status "目标架构: $TARGET_ARCH"
        fi
    else
        print_error "工具链可执行但运行失败"
        print_error "这可能是架构不兼容或依赖库缺失的问题"
        
        print_status "=== 诊断信息 ==="
        echo "文件信息:"
        file "${TOOLCHAIN_PREFIX}gcc"
        
        print_status "=== 建议的解决方案 ==="
        echo "1. 检查主机架构是否与工具链兼容"
        echo "2. 安装必要的 32 位库（如果工具链是 32 位的）:"
        echo "   sudo apt install libc6:i386 libncurses5:i386 libstdc++6:i386"
        echo "3. 尝试运行: ldd ${TOOLCHAIN_PREFIX}gcc"
        
        exit 1
    fi
}

# 检查并自动初始化子模块
check_and_init_submodules() {
    print_status "检查 Git 子模块状态..."
    
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
    
    # 检查子模块是否已经初始化和更新
    local need_init=false
    local need_update=false
    
    # 获取所有子模块路径
    local submodule_paths=($(git config --file .gitmodules --get-regexp path | awk '{ print $2 }'))
    
    if [ ${#submodule_paths[@]} -eq 0 ]; then
        print_warning "未找到任何子模块定义"
        return
    fi
    
    print_status "发现 ${#submodule_paths[@]} 个子模块，检查状态..."
    
    # 检查每个子模块的状态
    for submodule_path in "${submodule_paths[@]}"; do
        if [ ! -d "$submodule_path" ]; then
            print_warning "子模块目录不存在: $submodule_path"
            need_init=true
            continue
        fi
        
        # 检查子模块目录是否为空
        if [ -z "$(ls -A "$submodule_path" 2>/dev/null)" ]; then
            print_warning "子模块目录为空: $submodule_path"
            need_update=true
            continue
        fi
        
        # 检查子模块是否为有效的 git 仓库
        if [ ! -d "$submodule_path/.git" ] && [ ! -f "$submodule_path/.git" ]; then
            print_warning "子模块未正确初始化: $submodule_path"
            need_init=true
            need_update=true
            continue
        fi
        
        print_status "子模块状态正常: $submodule_path"
    done
    
    # 如果需要初始化子模块
    if [ "$need_init" = true ]; then
        print_status "检测到未初始化的子模块，正在自动初始化..."
        if git submodule init; then
            print_success "子模块初始化成功"
            need_update=true  # 初始化后需要更新
        else
            print_error "子模块初始化失败"
            print_error "请手动执行: git submodule init"
            exit 1
        fi
    fi
    
    # 如果需要更新子模块
    if [ "$need_update" = true ]; then
        print_status "正在更新子模块内容..."
        if git submodule update --recursive --init; then
            print_success "子模块更新成功"
        else
            print_error "子模块更新失败"
            print_error "请检查网络连接或手动执行: git submodule update --recursive --init"
            
            # 提供诊断信息
            echo ""
            print_status "=== 子模块诊断信息 ==="
            git submodule status
            echo ""
            print_status "可能的解决方案："
            echo "1. 检查网络连接是否正常"
            echo "2. 手动执行: git submodule update --recursive --init"
            echo "3. 如果是私有仓库，请确保有访问权限"
            echo "4. 尝试使用 HTTPS 替代 SSH: git config --global url.\"https://github.com/\".insteadOf git@github.com:"
            exit 1
        fi
    else
        print_success "所有子模块状态正常，无需更新"
    fi
    
    # 最终验证子模块状态
    print_status "验证子模块最终状态..."
    local all_ok=true
    for submodule_path in "${submodule_paths[@]}"; do
        if [ ! -d "$submodule_path" ] || [ -z "$(ls -A "$submodule_path" 2>/dev/null)" ]; then
            print_error "子模块验证失败: $submodule_path"
            all_ok=false
        fi
    done
    
    if [ "$all_ok" = true ]; then
        print_success "所有子模块验证通过"
        
        # 显示子模块状态摘要
        echo ""
        print_status "=== 子模块状态摘要 ==="
        git submodule status | while read line; do
            echo "  $line"
        done
    else
        print_error "部分子模块验证失败，请检查上述错误信息"
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
        print_error "请确保子模块已正确拉取"
        exit 1
    fi
    
    # 检查 fetch_lvgl.sh 脚本是否存在
    if [ ! -f "$LIBLVGL_DIR/fetch_lvgl.sh" ]; then
        print_error "fetch_lvgl.sh 脚本不存在: $LIBLVGL_DIR/fetch_lvgl.sh"
        print_error "请检查 liblvgl 子模块是否完整"
        exit 1
    fi
    
    # 检查 lvgl 目录是否已存在且不为空
    if [ -d "$LIBLVGL_DIR/lvgl" ] && [ -d "$LIBLVGL_DIR/lv_drivers" ]; then
        # 进一步检查目录是否有内容
        local lvgl_files=$(find "$LIBLVGL_DIR/lvgl" -name "*.c" -o -name "*.h" | wc -l)
        local driver_files=$(find "$LIBLVGL_DIR/lv_drivers" -name "*.c" -o -name "*.h" | wc -l)
        
        if [ "$lvgl_files" -gt 0 ] && [ "$driver_files" -gt 0 ]; then
            print_success "LVGL 源码已存在且完整 (LVGL: $lvgl_files 文件, 驱动: $driver_files 文件)"
            return
        else
            print_warning "LVGL 源码目录存在但内容不完整，重新获取..."
            rm -rf "$LIBLVGL_DIR/lvgl" "$LIBLVGL_DIR/lv_drivers"
        fi
    fi
    
    # 进入 liblvgl 目录并执行 fetch_lvgl.sh
    print_status "执行 fetch_lvgl.sh 脚本获取 LVGL 源码..."
    cd "$LIBLVGL_DIR"
    
    # 确保脚本有执行权限
    chmod +x fetch_lvgl.sh
    
    # 检查网络连接（通过尝试解析 GitHub）
    if ! nslookup github.com >/dev/null 2>&1; then
        print_warning "无法解析 github.com，网络可能有问题"
        print_status "如果下载失败，请检查网络连接或代理设置"
    fi
    
    # 执行获取脚本
    print_status "正在下载 LVGL 源码，这可能需要几分钟时间..."
    if timeout 300 ./fetch_lvgl.sh; then  # 5分钟超时
        print_success "LVGL 源码获取成功"
    else
        local exit_code=$?
        if [ $exit_code -eq 124 ]; then
            print_error "LVGL 源码下载超时（超过5分钟）"
        else
            print_error "LVGL 源码获取失败（退出码: $exit_code）"
        fi
        
        print_status "=== LVGL 源码获取失败的解决方案 ==="
        echo "1. 检查网络连接是否正常"
        echo "2. 如果在中国大陆，尝试配置代理："
        echo "   export https_proxy=http://proxy:port"
        echo "   export http_proxy=http://proxy:port"
        echo "3. 或者手动下载 LVGL 源码："
        echo "   cd $LIBLVGL_DIR"
        echo "   git clone https://github.com/lvgl/lvgl.git"
        echo "   git clone https://github.com/lvgl/lv_drivers.git"
        echo "4. 使用国内镜像（如果可用）："
        echo "   git config --global url.\"https://gitee.com/\".insteadOf \"https://github.com/\""
        
        cd "$PROJECT_ROOT"
        exit 1
    fi
    
    # 返回项目根目录
    cd "$PROJECT_ROOT"
    
    # 验证 LVGL 源码是否正确获取
    if [ -d "$LIBLVGL_DIR/lvgl" ] && [ -d "$LIBLVGL_DIR/lv_drivers" ]; then
        # 检查关键文件
        local lvgl_files=$(find "$LIBLVGL_DIR/lvgl" -name "*.c" -o -name "*.h" | wc -l)
        local driver_files=$(find "$LIBLVGL_DIR/lv_drivers" -name "*.c" -o -name "*.h" | wc -l)
        
        if [ "$lvgl_files" -gt 100 ] && [ "$driver_files" -gt 10 ]; then
            print_success "LVGL 源码验证成功 (LVGL: $lvgl_files 文件, 驱动: $driver_files 文件)"
        else
            print_error "LVGL 源码不完整 (LVGL: $lvgl_files 文件, 驱动: $driver_files 文件)"
            print_error "期望 LVGL 文件数 > 100，驱动文件数 > 10"
            exit 1
        fi
    else
        print_error "LVGL 源码验证失败，目录结构不正确"
        exit 1
    fi
}

# 检查子模块完整性
check_submodules() {
    print_status "检查子模块完整性..."
    
    LIB_SUBMODULES=("libgpio" "libmedia" "liblvgl" "libstaging")
    local all_ok=true
    local missing_modules=()
    local incomplete_modules=()
    
    for SUBMODULE in "${LIB_SUBMODULES[@]}"; do
        SUBMODULE_DIR="$PROJECT_ROOT/$SUBMODULE"
        
        # 检查目录是否存在
        if [ ! -d "$SUBMODULE_DIR" ]; then
            print_error "$SUBMODULE: 目录不存在"
            missing_modules+=("$SUBMODULE")
            all_ok=false
            continue
        fi
        
        # 检查 CMakeLists.txt 是否存在
        if [ ! -f "$SUBMODULE_DIR/CMakeLists.txt" ]; then
            print_error "$SUBMODULE: CMakeLists.txt 不存在"
            incomplete_modules+=("$SUBMODULE")
            all_ok=false
            continue
        fi
        
        # 检查源代码目录 (liblvgl 是特殊情况)
        local has_source=false
        if [ "$SUBMODULE" = "liblvgl" ]; then
            # liblvgl 的源码通过 fetch_lvgl.sh 获取到 lvgl/ 目录
            if [ -d "$SUBMODULE_DIR/lvgl" ] && [ -n "$(ls -A "$SUBMODULE_DIR/lvgl" 2>/dev/null)" ]; then
                has_source=true
            fi
        else
            # 其他模块检查标准的源码目录
            for source_dir in "source" "src"; do
                if [ -d "$SUBMODULE_DIR/$source_dir" ] && [ -n "$(ls -A "$SUBMODULE_DIR/$source_dir" 2>/dev/null)" ]; then
                    has_source=true
                    break
                fi
            done
        fi
        
        if [ "$has_source" = false ]; then
            print_warning "$SUBMODULE: 未找到源代码目录或目录为空"
            incomplete_modules+=("$SUBMODULE")
            all_ok=false
            continue
        fi
        
        # 检查头文件目录 (liblvgl 是特殊情况)
        local has_headers=false
        if [ "$SUBMODULE" = "liblvgl" ]; then
            # liblvgl 的头文件在 lvgl/ 目录中
            if [ -d "$SUBMODULE_DIR/lvgl" ] && [ -n "$(find "$SUBMODULE_DIR/lvgl" -name "*.h" 2>/dev/null)" ]; then
                has_headers=true
            fi
        else
            # 其他模块检查标准的头文件目录
            for header_dir in "include" "inc"; do
                if [ -d "$SUBMODULE_DIR/$header_dir" ] && [ -n "$(ls -A "$SUBMODULE_DIR/$header_dir" 2>/dev/null)" ]; then
                    has_headers=true
                    break
                fi
            done
        fi
        
        if [ "$has_headers" = false ]; then
            print_warning "$SUBMODULE: 未找到头文件目录或目录为空"
        fi
        
        # 统计文件数量
        local c_files=$(find "$SUBMODULE_DIR" -name "*.c" 2>/dev/null | wc -l)
        local h_files=$(find "$SUBMODULE_DIR" -name "*.h" 2>/dev/null | wc -l)
        
        print_success "$SUBMODULE: OK (C文件: $c_files, 头文件: $h_files)"
    done
    
    echo ""
    
    # 报告检查结果
    if [ "$all_ok" = true ]; then
        print_success "=== 所有子模块检查通过 ==="
    else
        print_error "=== 发现子模块问题 ==="
        
        if [ ${#missing_modules[@]} -gt 0 ]; then
            echo ""
            print_error "缺失的子模块:"
            for module in "${missing_modules[@]}"; do
                echo "  - $module"
            done
        fi
        
        if [ ${#incomplete_modules[@]} -gt 0 ]; then
            echo ""
            print_warning "不完整的子模块:"
            for module in "${incomplete_modules[@]}"; do
                echo "  - $module"
            done
        fi
        
        echo ""
        print_status "=== 建议的解决方案 ==="
        echo "1. 重新拉取子模块:"
        echo "   git submodule update --init --recursive --force"
        echo ""
        echo "2. 如果问题持续存在，尝试清理并重新克隆子模块:"
        echo "   git submodule deinit --all"
        echo "   git submodule update --init --recursive"
        echo ""
        echo "3. 检查 .gitmodules 文件中的子模块 URL 是否正确"
        echo ""
        echo "4. 如果是网络问题，尝试配置代理或使用镜像源"
        
        exit 1
    fi
}

# 清理编译目录
clean_build() {
    if [ "$FORCE_CLEAN" = true ]; then
        print_status "执行强制完全清理..."
        rm -rf build
        mkdir -p build
        mkdir -p build/lib
        mkdir -p build/bin
        return
    fi
    
    if [ "$CLEAN" = true ]; then
        print_status "清理编译目录..."
        rm -rf build
        mkdir -p build
        mkdir -p build/lib
        mkdir -p build/bin
        return
    fi
    
    # 增量编译模式：只创建必要的目录
    if [ ! -d "build" ]; then
        print_status "创建编译目录（首次编译）..."
        mkdir -p build
        mkdir -p build/lib
        mkdir -p build/bin
    else
        print_status "使用现有编译目录（增量编译模式）"
    fi
}

# 检查子模块是否需要重新编译
check_submodule_needs_rebuild() {
    local submodule="$1"
    local submodule_dir="$PROJECT_ROOT/$submodule"
    local submodule_build_dir="$PROJECT_ROOT/build/$submodule"
    local lib_pattern="$PROJECT_ROOT/build/lib/lib${submodule#lib}.*"
    
    # 如果是强制清理模式，总是需要重新编译
    if [ "$FORCE_CLEAN" = true ] || [ "$CLEAN" = true ]; then
        return 0  # 需要重新编译
    fi
    
    # 如果构建目录不存在，需要重新编译
    if [ ! -d "$submodule_build_dir" ]; then
        print_status "$submodule: 构建目录不存在，需要编译"
        return 0
    fi
    
    # 如果库文件不存在，需要重新编译
    if ! ls $lib_pattern &>/dev/null; then
        print_status "$submodule: 库文件不存在，需要编译"
        return 0
    fi
    
    # 获取最新的库文件时间
    local lib_time=$(stat -c %Y $lib_pattern 2>/dev/null | sort -n | tail -1)
    if [ -z "$lib_time" ]; then
        print_status "$submodule: 无法获取库文件时间，需要编译"
        return 0
    fi
    
    # 检查源代码是否有更新
    local source_dirs=()
    if [ "$submodule" = "liblvgl" ]; then
        source_dirs=("$submodule_dir/lvgl" "$submodule_dir/lv_drivers" "$submodule_dir")
    else
        source_dirs=("$submodule_dir/source" "$submodule_dir/src" "$submodule_dir/include" "$submodule_dir/inc" "$submodule_dir")
    fi
    
    local newest_source_time=0
    for source_dir in "${source_dirs[@]}"; do
        if [ -d "$source_dir" ]; then
            local dir_time=$(find "$source_dir" -name "*.c" -o -name "*.h" -o -name "*.cpp" -o -name "*.hpp" -o -name "CMakeLists.txt" 2>/dev/null | xargs stat -c %Y 2>/dev/null | sort -n | tail -1)
            if [ -n "$dir_time" ] && [ "$dir_time" -gt "$newest_source_time" ]; then
                newest_source_time="$dir_time"
            fi
        fi
    done
    
    if [ "$newest_source_time" -gt "$lib_time" ]; then
        print_status "$submodule: 源代码有更新，需要重新编译"
        return 0
    fi
    
    # 检查 CMakeLists.txt 是否有更新
    if [ -f "$submodule_dir/CMakeLists.txt" ]; then
        local cmake_time=$(stat -c %Y "$submodule_dir/CMakeLists.txt")
        if [ "$cmake_time" -gt "$lib_time" ]; then
            print_status "$submodule: CMakeLists.txt 有更新，需要重新编译"
            return 0
        fi
    fi
    
    print_status "$submodule: 无需重新编译（源码无变化）"
    return 1  # 不需要重新编译
}

# 编译子模块
build_submodules() {
    print_status "开始编译子模块（增量模式：$INCREMENTAL）..."
    
    LIB_SUBMODULES=("libgpio" "libmedia" "liblvgl" "libstaging")
    TOOLCHAIN_PREFIX_PATH="$PROJECT_ROOT/toolchains"
    
    local skipped_count=0
    local built_count=0
    
    for SUBMODULE in "${LIB_SUBMODULES[@]}"; do
        SUBMODULE_DIR="$PROJECT_ROOT/$SUBMODULE"
        SUBMODULE_BUILD_DIR="$PROJECT_ROOT/build/$SUBMODULE"
        
        if [ ! -d "$SUBMODULE_DIR" ] || [ ! -f "$SUBMODULE_DIR/CMakeLists.txt" ]; then
            print_warning "跳过 $SUBMODULE: 目录或 CMakeLists.txt 不存在"
            continue
        fi
        
        # 检查子模块是否需要重新编译
        if ! check_submodule_needs_rebuild "$SUBMODULE"; then
            print_success "$SUBMODULE: 跳过编译（无需重新编译）"
            ((skipped_count++))
            continue
        fi
        
        print_status "编译子模块: $SUBMODULE"
        ((built_count++))
        
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
    
    print_success "子模块编译完成 (编译: $built_count, 跳过: $skipped_count)"
    
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
    
    # 复制配置文件
    print_status "复制配置文件..."
    if [ -f "mxCamera_config_example.toml" ]; then
        cp mxCamera_config_example.toml "$PACKAGE_DIR/"
    else
        print_warning "mxCamera_config_example.toml 不存在，跳过"
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
- mxCamera_config_example.toml: 配置文件模板
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
3. 将 mxCamera_config_example.toml 复制到设备的 /root/Workspace/mxCamera_config.toml
4. 将 mxcamera 复制到设备的 /etc/init.d/S99mxcamera
5. 在设备上执行: chmod +x /root/Workspace/mxCamera
6. 在设备上执行: chmod +x /etc/init.d/S99mxcamera

配置说明：
=========
- 配置文件位置: /root/Workspace/mxCamera_config.toml
- 应用程序启动时会自动读取配置文件
- 如果配置文件不存在，会使用默认设置并自动创建配置文件
- 可以手动编辑配置文件来调整摄像头参数（曝光、增益、分辨率等）

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
    echo "增量编译: $INCREMENTAL"
    echo ""
    
    check_tools
    check_and_init_submodules
    check_toolchain
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
