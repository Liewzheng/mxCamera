cmake_minimum_required(VERSION 3.16)

# 项目信息
project(mxCamera
    VERSION 1.0.0
    DESCRIPTION "LVGL + libMedia Camera Real-time Display System"
    LANGUAGES C
)

# 设置 C 标准
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# 设置交叉编译工具链
set(TOOLCHAIN_PREFIX "/home/liewzheng/Workspace/luckfox-pico/tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf/bin/arm-rockchip830-linux-uclibcgnueabihf-")

# 设置编译器
set(CMAKE_C_COMPILER "${TOOLCHAIN_PREFIX}gcc")
set(CMAKE_CXX_COMPILER "${TOOLCHAIN_PREFIX}g++")
set(CMAKE_AR "${TOOLCHAIN_PREFIX}ar")
set(CMAKE_STRIP "${TOOLCHAIN_PREFIX}strip")
set(CMAKE_NM "${TOOLCHAIN_PREFIX}nm")
set(CMAKE_OBJCOPY "${TOOLCHAIN_PREFIX}objcopy")
set(CMAKE_OBJDUMP "${TOOLCHAIN_PREFIX}objdump")

# 设置交叉编译系统信息
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

# 编译选项
set(CMAKE_C_FLAGS_RELEASE "-O3 -DNDEBUG")
set(CMAKE_C_FLAGS_DEBUG "-O0 -g -DDEBUG")

# 默认为 Release 模式
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Choose the type of build" FORCE)
endif()

# 设置输出目录
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# ============================================================================
# 依赖库路径配置
# ============================================================================

# libgpio 路径
set(LIBGPIO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../libgpio")
set(LIBGPIO_INCLUDE_DIR "${LIBGPIO_DIR}/include")
set(LIBGPIO_LIB_DIR "${LIBGPIO_DIR}/build")

# libMedia 路径
set(LIBMEDIA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../libMedia")
set(LIBMEDIA_INCLUDE_DIR "${LIBMEDIA_DIR}/include")
set(LIBMEDIA_LIB_DIR "${LIBMEDIA_DIR}/build")

# LVGL 路径
set(LVGL_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../lvgl")
set(LVGL_DIR "${LVGL_ROOT}/lvgl")
set(LVGL_DRIVERS_DIR "${LVGL_ROOT}/lv_drivers")
set(LVGL_LIB_DIR "${LVGL_ROOT}/build")

# 项目头文件目录
set(PROJECT_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include")
set(PROJECT_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/source")

# ============================================================================
# 依赖检查
# ============================================================================

# 检查 libgpio
if(NOT EXISTS "${LIBGPIO_INCLUDE_DIR}/gpio.h")
    message(FATAL_ERROR "libgpio header not found at ${LIBGPIO_INCLUDE_DIR}")
endif()

find_library(LIBGPIO_LIBRARY 
    NAMES gpio
    PATHS ${LIBGPIO_LIB_DIR}
    NO_DEFAULT_PATH
)

if(NOT LIBGPIO_LIBRARY)
    message(FATAL_ERROR "libgpio library not found in ${LIBGPIO_LIB_DIR}")
endif()

# 检查 libMedia
if(NOT EXISTS "${LIBMEDIA_INCLUDE_DIR}/media.h")
    message(FATAL_ERROR "libMedia header not found at ${LIBMEDIA_INCLUDE_DIR}")
endif()

find_library(LIBMEDIA_LIBRARY 
    NAMES media
    PATHS ${LIBMEDIA_LIB_DIR}
    NO_DEFAULT_PATH
)

if(NOT LIBMEDIA_LIBRARY)
    message(FATAL_ERROR "libMedia library not found in ${LIBMEDIA_LIB_DIR}")
endif()

# 检查 LVGL
if(NOT EXISTS "${LVGL_DIR}")
    message(FATAL_ERROR "LVGL library not found at ${LVGL_DIR}")
endif()

if(NOT EXISTS "${LVGL_DRIVERS_DIR}")
    message(FATAL_ERROR "LVGL drivers not found at ${LVGL_DRIVERS_DIR}")
endif()

find_library(LVGL_LIBRARY 
    NAMES lvgl
    PATHS ${LVGL_LIB_DIR}
    NO_DEFAULT_PATH
)

if(NOT LVGL_LIBRARY)
    message(FATAL_ERROR "LVGL library not found in ${LVGL_LIB_DIR}")
endif()

# ============================================================================
# 头文件搜索路径
# ============================================================================

include_directories(
    ${PROJECT_INCLUDE_DIR}
    ${LIBGPIO_INCLUDE_DIR}
    ${LIBMEDIA_INCLUDE_DIR}
    ${LVGL_DIR}
    ${LVGL_DRIVERS_DIR}
    ${LVGL_ROOT}
)

# ============================================================================
# 编译标志
# ============================================================================

set(COMMON_FLAGS
    -Wall -Wshadow -Wundef -Wmissing-prototypes -Wno-discarded-qualifiers
    -Wextra -Wno-unused-function -Wno-error=strict-prototypes -Wpointer-arith
    -fno-strict-aliasing -Wno-error=cpp -Wuninitialized -Wmaybe-uninitialized
    -Wno-unused-parameter -Wno-missing-field-initializers -Wtype-limits
    -Wsizeof-pointer-memaccess -Wno-format-nonliteral -Wno-cast-qual
    -Wunreachable-code -Wno-switch-default -Wreturn-type -Wmultichar
    -Wformat-security -Wno-ignored-qualifiers -Wno-error=pedantic
    -Wno-sign-compare -Wno-error=missing-prototypes -Wdouble-promotion
    -Wclobbered -Wdeprecated -Wempty-body -Wshift-negative-value
    -Wstack-usage=2048 -Wno-unused-value
)

add_compile_options(${COMMON_FLAGS})

# ============================================================================
# 创建导入的动态库目标
# ============================================================================

# libgpio
add_library(gpio SHARED IMPORTED)
set_target_properties(gpio PROPERTIES
    IMPORTED_LOCATION ${LIBGPIO_LIBRARY}
    INTERFACE_INCLUDE_DIRECTORIES ${LIBGPIO_INCLUDE_DIR}
)

# libMedia
add_library(media SHARED IMPORTED)
set_target_properties(media PROPERTIES
    IMPORTED_LOCATION ${LIBMEDIA_LIBRARY}
    INTERFACE_INCLUDE_DIRECTORIES ${LIBMEDIA_INCLUDE_DIR}
)

# LVGL
add_library(lvgl SHARED IMPORTED)
set_target_properties(lvgl PROPERTIES
    IMPORTED_LOCATION ${LVGL_LIBRARY}
    INTERFACE_INCLUDE_DIRECTORIES "${LVGL_DIR};${LVGL_DRIVERS_DIR};${LVGL_ROOT}"
)

# ============================================================================
# 项目源文件
# ============================================================================

# 收集项目源文件
file(GLOB PROJECT_SOURCES 
    "${PROJECT_SOURCE_DIR}/*.c"
)

# 创建可执行文件
add_executable(${PROJECT_NAME} ${PROJECT_SOURCES})

# 设置包含目录
target_include_directories(${PROJECT_NAME} PRIVATE
    ${PROJECT_INCLUDE_DIR}
    ${LIBGPIO_INCLUDE_DIR}
    ${LIBMEDIA_INCLUDE_DIR}
    ${LVGL_DIR}
    ${LVGL_DRIVERS_DIR}
    ${LVGL_ROOT}
)

# 链接库
target_link_libraries(${PROJECT_NAME} 
    gpio      # libgpio 动态库
    media     # libMedia 动态库
    lvgl      # LVGL 动态库
    m         # 数学库
    pthread   # 线程库
)

# 设置可执行文件属性
set_target_properties(${PROJECT_NAME} PROPERTIES
    OUTPUT_NAME "mxCamera"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# ============================================================================
# 安装规则
# ============================================================================

install(TARGETS ${PROJECT_NAME}
    RUNTIME DESTINATION bin
)

# ============================================================================
# 自定义目标
# ============================================================================

# 添加清理目标
add_custom_target(clean-all
    COMMAND ${CMAKE_BUILD_TOOL} clean
    COMMAND ${CMAKE_COMMAND} -E remove_directory ${CMAKE_BINARY_DIR}
    COMMENT "Cleaning all build files"
)

# 添加信息显示目标
add_custom_target(info
    COMMAND ${CMAKE_COMMAND} -E echo "=== 项目信息 ==="
    COMMAND ${CMAKE_COMMAND} -E echo "项目名称: ${PROJECT_NAME}"
    COMMAND ${CMAKE_COMMAND} -E echo "版本号: ${PROJECT_VERSION}"
    COMMAND ${CMAKE_COMMAND} -E echo "编译类型: ${CMAKE_BUILD_TYPE}"
    COMMAND ${CMAKE_COMMAND} -E echo "编译器: ${CMAKE_C_COMPILER}"
    COMMAND ${CMAKE_COMMAND} -E echo "输出目录: ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
    COMMAND ${CMAKE_COMMAND} -E echo "libgpio: ${LIBGPIO_LIBRARY}"
    COMMAND ${CMAKE_COMMAND} -E echo "libMedia: ${LIBMEDIA_LIBRARY}"
    COMMAND ${CMAKE_COMMAND} -E echo "LVGL: ${LVGL_LIBRARY}"
    COMMENT "显示项目信息"
)

# 添加部署目标
add_custom_target(deploy
    DEPENDS ${PROJECT_NAME}
    COMMAND ${CMAKE_COMMAND} -E echo "=== 部署说明 ==="
    COMMAND ${CMAKE_COMMAND} -E echo "1. 将可执行文件和库复制到目标设备:"
    COMMAND ${CMAKE_COMMAND} -E echo "   scp ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/mxCamera root@<target_ip>:~/"
    COMMAND ${CMAKE_COMMAND} -E echo "   scp ${LIBGPIO_LIBRARY} root@<target_ip>:/usr/lib/"
    COMMAND ${CMAKE_COMMAND} -E echo "   scp ${LIBMEDIA_LIBRARY} root@<target_ip>:/usr/lib/"
    COMMAND ${CMAKE_COMMAND} -E echo "   scp ${LVGL_LIBRARY} root@<target_ip>:/usr/lib/"
    COMMAND ${CMAKE_COMMAND} -E echo "2. 在目标设备上运行:"
    COMMAND ${CMAKE_COMMAND} -E echo "   export LD_LIBRARY_PATH=/usr/lib:\$LD_LIBRARY_PATH"
    COMMAND ${CMAKE_COMMAND} -E echo "   chmod +x mxCamera"
    COMMAND ${CMAKE_COMMAND} -E echo "   ./mxCamera"
    COMMENT "显示部署说明"
)

# ============================================================================
# 打包配置
# ============================================================================

set(CPACK_PACKAGE_NAME "${PROJECT_NAME}")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
set(CPACK_PACKAGE_VENDOR "Luckfox Team")
set(CPACK_GENERATOR "TGZ")
set(CPACK_PACKAGE_FILE_NAME "${PROJECT_NAME}-${PROJECT_VERSION}-arm")

include(CPack)

# ============================================================================
# 调试信息
# ============================================================================

message(STATUS "=== mxCamera 项目配置 ===")
message(STATUS "项目名称: ${PROJECT_NAME}")
message(STATUS "版本: ${PROJECT_VERSION}")
message(STATUS "编译类型: ${CMAKE_BUILD_TYPE}")
message(STATUS "libgpio: ${LIBGPIO_LIBRARY}")
message(STATUS "libMedia: ${LIBMEDIA_LIBRARY}")
message(STATUS "LVGL: ${LVGL_LIBRARY}")
message(STATUS "输出目录: ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
