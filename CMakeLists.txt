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

# 设置交叉编译工具链 - 使用本地 toolchains 目录
set(TOOLCHAIN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/toolchains")
set(TOOLCHAIN_PREFIX "${TOOLCHAIN_DIR}/bin/arm-rockchip830-linux-uclibcgnueabihf-")

# 检查工具链是否存在
if(NOT EXISTS "${TOOLCHAIN_PREFIX}gcc")
    message(FATAL_ERROR "Toolchain not found at ${TOOLCHAIN_PREFIX}gcc")
endif()

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

# 设置统一的输出目录 - 所有库输出到 mxCamera/build/lib
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# 强制所有子项目使用统一的输出目录
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_DEBUG ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_DEBUG ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/lib)

# ============================================================================
# 子模块编译配置
# ============================================================================

# 定义所有需要编译的 lib 子模块
set(LIB_SUBMODULES
    libgpio
    libmedia
    liblvgl
    libstaging
)

# 为所有子模块设置统一的编译配置
foreach(SUBMODULE ${LIB_SUBMODULES})
    set(SUBMODULE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/${SUBMODULE}")
    
    if(EXISTS "${SUBMODULE_DIR}/CMakeLists.txt")
        message(STATUS "配置子模块: ${SUBMODULE}")
        
        # 创建子模块的构建目录
        set(SUBMODULE_BUILD_DIR "${CMAKE_BINARY_DIR}/${SUBMODULE}")
        file(MAKE_DIRECTORY ${SUBMODULE_BUILD_DIR})
        
        # 为子模块设置统一的工具链和输出目录
        set(SUBMODULE_CMAKE_ARGS
            -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
            -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
            -DCMAKE_AR=${CMAKE_AR}
            -DCMAKE_STRIP=${CMAKE_STRIP}
            -DCMAKE_NM=${CMAKE_NM}
            -DCMAKE_OBJCOPY=${CMAKE_OBJCOPY}
            -DCMAKE_OBJDUMP=${CMAKE_OBJDUMP}
            -DCMAKE_SYSTEM_NAME=Linux
            -DCMAKE_SYSTEM_PROCESSOR=arm
            -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
            -DCMAKE_LIBRARY_OUTPUT_DIRECTORY=${CMAKE_LIBRARY_OUTPUT_DIRECTORY}
            -DCMAKE_ARCHIVE_OUTPUT_DIRECTORY=${CMAKE_ARCHIVE_OUTPUT_DIRECTORY}
            -DCMAKE_LIBRARY_OUTPUT_DIRECTORY_DEBUG=${CMAKE_LIBRARY_OUTPUT_DIRECTORY}
            -DCMAKE_LIBRARY_OUTPUT_DIRECTORY_RELEASE=${CMAKE_LIBRARY_OUTPUT_DIRECTORY}
            -DCMAKE_ARCHIVE_OUTPUT_DIRECTORY_DEBUG=${CMAKE_ARCHIVE_OUTPUT_DIRECTORY}
            -DCMAKE_ARCHIVE_OUTPUT_DIRECTORY_RELEASE=${CMAKE_ARCHIVE_OUTPUT_DIRECTORY}
        )
        
        # 特殊处理某些子模块的额外参数
        if(${SUBMODULE} STREQUAL "libgpio")
            list(APPEND SUBMODULE_CMAKE_ARGS -DENABLE_CROSS_COMPILE=ON)
        elseif(${SUBMODULE} STREQUAL "libmedia")
            list(APPEND SUBMODULE_CMAKE_ARGS -DENABLE_CROSS_COMPILE=ON)
        endif()
        
        # 使用 ExternalProject 编译子模块
        include(ExternalProject)
        ExternalProject_Add(${SUBMODULE}_build
            SOURCE_DIR ${SUBMODULE_DIR}
            BINARY_DIR ${SUBMODULE_BUILD_DIR}
            CMAKE_ARGS ${SUBMODULE_CMAKE_ARGS}
            BUILD_ALWAYS 1
            INSTALL_COMMAND ""  # 不执行安装，直接使用构建输出
        )
        
        # 记录子模块信息用于后续依赖
        set(${SUBMODULE}_BUILT_DIR ${SUBMODULE_BUILD_DIR})
        
    else()
        message(WARNING "子模块 ${SUBMODULE} 的 CMakeLists.txt 不存在")
    endif()
endforeach()

# ============================================================================
# 依赖库路径配置
# ============================================================================

# libgpio 路径 - 使用统一的构建输出
set(LIBGPIO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/libgpio")
set(LIBGPIO_INCLUDE_DIR "${LIBGPIO_DIR}/include")
set(LIBGPIO_LIB_DIR "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}")

# libMedia 路径 - 使用统一的构建输出
set(LIBMEDIA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/libmedia")
set(LIBMEDIA_INCLUDE_DIR "${LIBMEDIA_DIR}/include")
set(LIBMEDIA_LIB_DIR "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}")

# libstaging 路径 - 使用统一的构建输出
set(LIBSTAGING_DIR "${CMAKE_CURRENT_SOURCE_DIR}/libstaging")
set(LIBSTAGING_INCLUDE_DIR "${LIBSTAGING_DIR}/include")
set(LIBSTAGING_LIB_DIR "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}")

# LVGL 路径 - 使用统一的构建输出
set(LVGL_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/liblvgl")
set(LVGL_DIR "${LVGL_ROOT}/lvgl")
set(LVGL_DRIVERS_DIR "${LVGL_ROOT}/lv_drivers")
set(LVGL_LIB_DIR "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}")

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
message(STATUS "libgpio 头文件检查: OK")

# 检查 libMedia
if(NOT EXISTS "${LIBMEDIA_INCLUDE_DIR}/media.h")
    message(FATAL_ERROR "libMedia header not found at ${LIBMEDIA_INCLUDE_DIR}")
endif()
message(STATUS "libMedia 头文件检查: OK")

# 检查 libstaging
if(NOT EXISTS "${LIBSTAGING_INCLUDE_DIR}/fbtft_lcd.h")
    message(FATAL_ERROR "libstaging header not found at ${LIBSTAGING_INCLUDE_DIR}")
endif()
message(STATUS "libstaging 头文件检查: OK")

# 检查 LVGL
# 检查 LVGL - 如果不存在则自动获取
if(NOT EXISTS "${LVGL_DIR}")
    message(STATUS "LVGL 源码不存在，尝试自动获取...")
    
    # 检查 fetch_lvgl.sh 脚本是否存在
    set(FETCH_LVGL_SCRIPT "${LVGL_ROOT}/fetch_lvgl.sh")
    if(EXISTS "${FETCH_LVGL_SCRIPT}")
        message(STATUS "执行 fetch_lvgl.sh 获取 LVGL 源码...")
        
        # 执行 fetch_lvgl.sh 脚本
        execute_process(
            COMMAND chmod +x ${FETCH_LVGL_SCRIPT}
            WORKING_DIRECTORY ${LVGL_ROOT}
            RESULT_VARIABLE CHMOD_RESULT
        )
        
        execute_process(
            COMMAND ${FETCH_LVGL_SCRIPT}
            WORKING_DIRECTORY ${LVGL_ROOT}
            RESULT_VARIABLE FETCH_RESULT
            OUTPUT_VARIABLE FETCH_OUTPUT
            ERROR_VARIABLE FETCH_ERROR
        )
        
        if(FETCH_RESULT EQUAL 0)
            message(STATUS "LVGL 源码获取成功")
        else()
            message(FATAL_ERROR "LVGL 源码获取失败: ${FETCH_ERROR}")
        endif()
    else()
        message(FATAL_ERROR "fetch_lvgl.sh 脚本不存在: ${FETCH_LVGL_SCRIPT}")
    endif()
endif()

if(NOT EXISTS "${LVGL_DIR}")
    message(FATAL_ERROR "LVGL library not found at ${LVGL_DIR}")
endif()

if(NOT EXISTS "${LVGL_DRIVERS_DIR}")
    message(FATAL_ERROR "LVGL drivers not found at ${LVGL_DRIVERS_DIR}")
endif()

message(STATUS "LVGL 源码检查: OK")

# ============================================================================
# 头文件搜索路径
# ============================================================================

include_directories(
    ${PROJECT_INCLUDE_DIR}
    ${LIBGPIO_INCLUDE_DIR}
    ${LIBMEDIA_INCLUDE_DIR}
    ${LIBSTAGING_INCLUDE_DIR}
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
# 创建导入的动态库目标 - 指向将要编译生成的库文件
# ============================================================================

# libgpio
add_library(gpio SHARED IMPORTED)
set_target_properties(gpio PROPERTIES
    IMPORTED_LOCATION ${LIBGPIO_LIB_DIR}/libgpio.so
    INTERFACE_INCLUDE_DIRECTORIES ${LIBGPIO_INCLUDE_DIR}
)

# libMedia
add_library(media SHARED IMPORTED)
set_target_properties(media PROPERTIES
    IMPORTED_LOCATION ${LIBMEDIA_LIB_DIR}/libmedia.so
    INTERFACE_INCLUDE_DIRECTORIES ${LIBMEDIA_INCLUDE_DIR}
)

# libstaging
add_library(staging SHARED IMPORTED)
set_target_properties(staging PROPERTIES
    IMPORTED_LOCATION ${LIBSTAGING_LIB_DIR}/libstaging.so
    INTERFACE_INCLUDE_DIRECTORIES ${LIBSTAGING_INCLUDE_DIR}
)

# LVGL
add_library(lvgl SHARED IMPORTED)
set_target_properties(lvgl PROPERTIES
    IMPORTED_LOCATION ${LVGL_LIB_DIR}/liblvgl.so
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

# 添加子模块依赖 - 确保子模块先编译完成
add_dependencies(${PROJECT_NAME} 
    libgpio_build
    libmedia_build
    liblvgl_build
    libstaging_build
)

# 设置包含目录
target_include_directories(${PROJECT_NAME} PRIVATE
    ${PROJECT_INCLUDE_DIR}
    ${LIBGPIO_INCLUDE_DIR}
    ${LIBMEDIA_INCLUDE_DIR}
    ${LIBSTAGING_INCLUDE_DIR}
    ${LVGL_DIR}
    ${LVGL_DRIVERS_DIR}
    ${LVGL_ROOT}
)

# 链接库
target_link_libraries(${PROJECT_NAME} 
    gpio      # libgpio 动态库
    media     # libMedia 动态库
    staging   # libstaging 动态库 (fbtft_benchmark)
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
    COMMAND ${CMAKE_COMMAND} -E echo "libstaging: ${LIBSTAGING_LIBRARY}"
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
    COMMAND ${CMAKE_COMMAND} -E echo "   scp ${LIBSTAGING_LIBRARY} root@<target_ip>:/usr/lib/"
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
message(STATUS "工具链目录: ${TOOLCHAIN_DIR}")
message(STATUS "编译器: ${CMAKE_C_COMPILER}")
message(STATUS "统一输出目录: ${CMAKE_LIBRARY_OUTPUT_DIRECTORY}")
message(STATUS "子模块编译状态:")
foreach(SUBMODULE ${LIB_SUBMODULES})
    message(STATUS "  - ${SUBMODULE}: 已配置")
endforeach()
message(STATUS "libgpio 库路径: ${LIBGPIO_LIB_DIR}/libgpio.so")
message(STATUS "libMedia 库路径: ${LIBMEDIA_LIB_DIR}/libmedia.so")
message(STATUS "libstaging 库路径: ${LIBSTAGING_LIB_DIR}/libstaging.so")
message(STATUS "LVGL 库路径: ${LVGL_LIB_DIR}/liblvgl.so")
message(STATUS "可执行文件输出: ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
