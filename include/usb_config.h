/**
 * @file usb_config.h
 * @brief USB配置管理模块头文件
 * @details 提供USB设备模式切换功能的接口定义
 */

#ifndef USB_CONFIG_H
#define USB_CONFIG_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// 类型定义
// ============================================================================

/**
 * @brief USB模式枚举
 */
typedef enum {
    USB_MODE_ADB,           /**< Android Debug Bridge */
    USB_MODE_RNDIS,         /**< RNDIS网络模式 (TCP可用) */
    USB_MODE_ACM,           /**< Abstract Control Model (串口) */
    USB_MODE_UVC,           /**< USB Video Class (摄像头) */
    USB_MODE_UMS,           /**< USB Mass Storage */
    USB_MODE_COUNT          /**< 模式总数 */
} usb_mode_t;

// ============================================================================
// 函数声明
// ============================================================================

/**
 * @brief 初始化USB配置模块
 * @return 0成功，-1失败
 */
int init_usb_config(void);

/**
 * @brief 清理USB配置模块
 */
void cleanup_usb_config(void);

/**
 * @brief 设置USB模式
 * @param mode USB模式
 * @return 0成功，-1失败
 */
int set_usb_mode(usb_mode_t mode);

/**
 * @brief 获取当前USB模式
 * @return 当前USB模式
 */
usb_mode_t get_usb_mode(void);

/**
 * @brief 获取USB模式名称
 * @param mode USB模式
 * @return 模式名称字符串
 */
const char* get_usb_mode_name(usb_mode_t mode);

/**
 * @brief 获取下一个USB模式（循环）
 * @param current_mode 当前模式
 * @return 下一个模式
 */
usb_mode_t get_next_usb_mode(usb_mode_t current_mode);

/**
 * @brief 获取上一个USB模式（循环）
 * @param current_mode 当前模式
 * @return 上一个模式
 */
usb_mode_t get_prev_usb_mode(usb_mode_t current_mode);

/**
 * @brief 检查TCP是否可用（只有RNDIS模式下TCP才可用）
 * @return true TCP可用，false TCP不可用
 */
bool is_tcp_available(void);

/**
 * @brief 获取USB模式描述信息
 * @param mode USB模式
 * @return 描述信息字符串
 */
const char* get_usb_mode_description(usb_mode_t mode);

/**
 * @brief 重启USB配置（停止后重新启动）
 * @return 0成功，-1失败
 */
int restart_usb_config(void);

#ifdef __cplusplus
}
#endif

#endif // USB_CONFIG_H
