/**
 * @file usb_config.c
 * @brief USB配置管理模块
 * @details 提供USB设备模式切换功能，支持ACM/ADB/UMS/UVC/RNDIS模式
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>

#include "usb_config.h"

// ============================================================================
// 全局变量
// ============================================================================

static usb_mode_t current_usb_mode = USB_MODE_ADB;  // 默认为ADB模式
static const char* USB_CONFIG_SCRIPT = "/etc/init.d/S50usbdevice";
static const char* USB_MODE_FILE = "/tmp/usb_mode_state";

// USB模式名称映射表
static const char* usb_mode_names[] = {
    "ADB",      // USB_MODE_ADB  
    "RNDIS",    // USB_MODE_RNDIS
    "ACM",      // USB_MODE_ACM
    "UVC",      // USB_MODE_UVC
    "UMS"       // USB_MODE_UMS
};

// USB模式配置命令映射表
static const char* usb_mode_commands[] = {
    "adb",      // USB_MODE_ADB
    "rndis",    // USB_MODE_RNDIS
    "acm",      // USB_MODE_ACM
    "uvc",      // USB_MODE_UVC
    "ums"       // USB_MODE_UMS
};

// ============================================================================
// 内部函数声明
// ============================================================================

static int execute_usb_command(const char* command);
static int save_usb_mode(usb_mode_t mode);
static usb_mode_t load_usb_mode(void);
static bool is_script_available(void);

// ============================================================================
// 公共函数实现
// ============================================================================

/**
 * @brief 初始化USB配置模块
 */
int init_usb_config(void) {
    printf("Initializing USB configuration module...\n");
    
    // 检查S50usbdevice脚本是否可用
    if (!is_script_available()) {
        printf("Warning: USB configuration script not available: %s\n", USB_CONFIG_SCRIPT);
        return -1;
    }
    
    // 加载保存的USB模式
    current_usb_mode = load_usb_mode();
    printf("USB configuration initialized, current mode: %s\n", 
           get_usb_mode_name(current_usb_mode));
    
    return 0;
}

/**
 * @brief 清理USB配置模块
 */
void cleanup_usb_config(void) {
    printf("Cleaning up USB configuration module...\n");
    
    // 保存当前USB模式
    save_usb_mode(current_usb_mode);
    
    printf("USB configuration cleanup complete\n");
}

/**
 * @brief 设置USB模式
 */
int set_usb_mode(usb_mode_t mode) {
    if (mode < 0 || mode >= USB_MODE_COUNT) {
        printf("Error: Invalid USB mode: %d\n", mode);
        return -1;
    }
    
    printf("Setting USB mode to: %s\n", get_usb_mode_name(mode));
    
    // 构建命令
    char command[256];
    snprintf(command, sizeof(command), "%s %s", 
             USB_CONFIG_SCRIPT, usb_mode_commands[mode]);
    
    // 执行USB配置命令
    int result = execute_usb_command(command);
    if (result != 0) {
        printf("Error: Failed to set USB mode to %s\n", get_usb_mode_name(mode));
        return -1;
    }
    
    // 更新当前模式
    current_usb_mode = mode;
    
    // 保存模式状态
    save_usb_mode(mode);
    
    printf("USB mode set successfully: %s\n", get_usb_mode_name(mode));
    return 0;
}

/**
 * @brief 获取当前USB模式
 */
usb_mode_t get_usb_mode(void) {
    return current_usb_mode;
}

/**
 * @brief 获取USB模式名称
 */
const char* get_usb_mode_name(usb_mode_t mode) {
    if (mode < 0 || mode >= USB_MODE_COUNT) {
        return "UNKNOWN";
    }
    return usb_mode_names[mode];
}

/**
 * @brief 获取下一个USB模式（循环）
 */
usb_mode_t get_next_usb_mode(usb_mode_t current_mode) {
    int next = (current_mode + 1) % USB_MODE_COUNT;
    return (usb_mode_t)next;
}

/**
 * @brief 获取上一个USB模式（循环）
 */
usb_mode_t get_prev_usb_mode(usb_mode_t current_mode) {
    int prev = (current_mode - 1 + USB_MODE_COUNT) % USB_MODE_COUNT;
    return (usb_mode_t)prev;
}

/**
 * @brief 检查TCP是否可用（只有RNDIS模式下TCP才可用）
 */
bool is_tcp_available(void) {
    return (current_usb_mode == USB_MODE_RNDIS);
}

/**
 * @brief 获取USB模式描述信息
 */
const char* get_usb_mode_description(usb_mode_t mode) {
    switch (mode) {
        case USB_MODE_ACM:
            return "Serial port communication";
        case USB_MODE_ADB:
            return "Android Debug Bridge";
        case USB_MODE_UMS:
            return "USB Mass Storage";
        case USB_MODE_UVC:
            return "USB Video Class camera";
        case USB_MODE_RNDIS:
            return "Network over USB (TCP enabled)";
        default:
            return "Unknown mode";
    }
}

/**
 * @brief 重启USB配置（停止后重新启动）
 */
int restart_usb_config(void) {
    printf("Restarting USB configuration...\n");
    
    // 构建重启命令
    char command[256];
    snprintf(command, sizeof(command), "%s restart", USB_CONFIG_SCRIPT);
    
    // 执行重启命令
    int result = execute_usb_command(command);
    if (result != 0) {
        printf("Error: Failed to restart USB configuration\n");
        return -1;
    }
    
    printf("USB configuration restarted successfully\n");
    return 0;
}

// ============================================================================
// 内部函数实现
// ============================================================================

/**
 * @brief 执行USB配置命令
 */
static int execute_usb_command(const char* command) {
    if (!command) {
        printf("Error: Invalid command\n");
        return -1;
    }
    
    printf("Executing USB command: %s\n", command);
    
    int status = system(command);
    
    if (status == -1) {
        printf("Error: Failed to execute command: %s\n", strerror(errno));
        return -1;
    }
    
    if (WIFEXITED(status)) {
        int exit_code = WEXITSTATUS(status);
        if (exit_code != 0) {
            printf("Error: Command failed with exit code: %d\n", exit_code);
            return -1;
        }
    } else {
        printf("Error: Command terminated abnormally\n");
        return -1;
    }
    
    return 0;
}

/**
 * @brief 保存USB模式到文件
 */
static int save_usb_mode(usb_mode_t mode) {
    FILE* file = fopen(USB_MODE_FILE, "w");
    if (!file) {
        printf("Warning: Failed to save USB mode: %s\n", strerror(errno));
        return -1;
    }
    
    fprintf(file, "%d\n", mode);
    fclose(file);
    
    return 0;
}

/**
 * @brief 从文件加载USB模式
 */
static usb_mode_t load_usb_mode(void) {
    FILE* file = fopen(USB_MODE_FILE, "r");
    if (!file) {
        // 文件不存在，返回默认模式
        return USB_MODE_ADB;
    }
    
    int mode;
    if (fscanf(file, "%d", &mode) != 1) {
        fclose(file);
        return USB_MODE_ADB;
    }
    
    fclose(file);
    
    // 验证模式范围
    if (mode < 0 || mode >= USB_MODE_COUNT) {
        return USB_MODE_ADB;
    }
    
    return (usb_mode_t)mode;
}

/**
 * @brief 检查USB配置脚本是否可用
 */
static bool is_script_available(void) {
    return (access(USB_CONFIG_SCRIPT, X_OK) == 0);
}
