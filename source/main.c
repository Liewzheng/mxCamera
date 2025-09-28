/**
 * @file main.c
 * @brief LVGL + libMedia 摄像头实时显示系统
 *
 * 功能：
 * - 使用 libMedia 库采集摄像头 RAW10 图像
 * - 通过 LVGL 将图像缩放显示到屏幕
 * - 实时显示帧率信息
 * - 通过按键控制摄像头启停
 *
 * 依赖库：
 * - libgpio.so (GPIO控制)
 * - libmedia.so (摄像头采集)
 * - liblvgl.so (图形界面)
 * - libstaging.so (图像处理，来自 fbtft_benchmark)
 */

// 定义 GNU 扩展以支持 pthread_timedjoin_np
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <pthread.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <ctype.h>
#include <stdbool.h>
#include <linux/videodev2.h>

// 库头文件
#include <gpio.h>
#include <media.h>
#include <subsys.h> // 子系统通信库
#include "DEV_Config.h"
#include "lv_drivers/display/fbdev.h"
#include "lvgl/lvgl.h"
#include "fbtft_lcd.h"

// TCP 传输相关数据结构 (参考 media_usb)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>  // 为TCP_NODELAY添加
#include <sys/socket.h>
#include <sys/select.h>

#include "mxCamera.h"
#include "usb_config.h" // USB配置管理

// ============================================================================
// 系统配置常量
// ============================================================================

// 摄像头配置 (默认值，可通过命令行参数覆盖)
#define DEFAULT_CAMERA_WIDTH 1920
#define DEFAULT_CAMERA_HEIGHT 1080
#define CAMERA_PIXELFORMAT V4L2_PIX_FMT_SBGGR10
#define DEFAULT_CAMERA_DEVICE "/dev/video0"
#define BUFFER_COUNT 2 // 增加缓冲区数量以减少帧丢失，提高帧率稳定性

// 全局摄像头配置变量 (可通过命令行修改)
static int camera_width = DEFAULT_CAMERA_WIDTH;
static int camera_height = DEFAULT_CAMERA_HEIGHT;

// 裁剪参数
static int crop_top = 0;
static int crop_left = 0;

// 显示配置 according to "fbtft_lcd.h"
#define DISPLAY_WIDTH FBTFT_LCD_DEFAULT_WIDTH
#define DISPLAY_HEIGHT FBTFT_LCD_DEFAULT_HEIGHT

#define DISP_BUF_SIZE (DISPLAY_WIDTH * DISPLAY_HEIGHT)

// TCP 传输配置 (参考 media_usb)
#define DEFAULT_PORT 8888
#define DEFAULT_SERVER_IP "172.32.0.93"
#define HEADER_SIZE 32
#define CHUNK_SIZE 1048576  // 增加到1MB以减少系统调用次数，提高传输效率

// 动态图像尺寸 (根据摄像头宽高比计算)
static int current_img_width = DISPLAY_WIDTH;
static int current_img_height = DISPLAY_HEIGHT;

// 帧率统计配置 (重新定义以避免冲突)
#undef FPS_UPDATE_INTERVAL
#define FPS_UPDATE_INTERVAL 1000000 // 1秒 (微秒)，稳定的帧率统计间隔

// 配置文件相关常量
#define CONFIG_FILE_PATH "/root/Workspace/mxCamera_config.toml"
#define CONFIG_MAX_LINE_LENGTH 256
#define CONFIG_MAX_KEY_LENGTH 64
#define CONFIG_MAX_VALUE_LENGTH 128

#define CONFIG_IMAGE_PATH "/mnt/ums/images"
#define CONFIG_TIME_BASE_YEAR 1955
#define CONFIG_TIME_BASE_MONTH 8
#define CONFIG_TIME_BASE_DAY 5

// 电量显示控制开关，如果不需要显示则设置为 0
#define BATTERY_SHOW 0

// ============================================================================
// 全局变量
// ============================================================================

// 系统状态
static volatile int exit_flag = 0;
static volatile int camera_paused = 0;   // 摄像头采集暂停状态
static volatile int display_enabled = 1; // 图像显示开关状态 (KEY0控制)
static volatile int tcp_enabled = 0;
static volatile int screen_on = 1;          // 屏幕开关状态
static volatile int menu_visible = 0;       // 设置菜单显示状态
static volatile int menu_selected_item = 0; // 菜单选中项 (参见 MENU_ITEM_* 常量)

enum
{
    MENU_ITEM_TCP = 0,
    MENU_ITEM_DISPLAY,
    MENU_ITEM_EXPOSURE,
    MENU_ITEM_GAIN,
    MENU_ITEM_USB,
    MENU_ITEM_HEATER1,
    MENU_ITEM_HEATER2,
    MENU_ITEM_PUMP,
    MENU_ITEM_LASER,
    MENU_ITEM_COUNT
};
static volatile int in_adjustment_mode = 0; // 是否在调整模式中
static volatile int adjustment_type = 0;    // 调整类型 (0=exposure, 1=gain)
static struct timeval last_activity_time;   // 最后活动时间
static struct timeval last_time_update;     // 时间显示更新时间戳

// LCD设备管理
static fbtft_lcd_t lcd_device;  // LCD设备结构体
static int lcd_initialized = 0; // LCD设备初始化状态

// 相机控制状态
static int subdev_handle = -1;         // 子设备句柄
static int32_t current_exposure = 128; // 当前曝光值 (1-1352)
static int32_t current_gain = 128;     // 当前增益值 (128-99614)
static int32_t exposure_min = 1;       // 曝光最小值
static int32_t exposure_max = 1352;    // 曝光最大值
static int32_t gain_min = 128;         // 增益最小值
static int32_t gain_max = 99614;       // 增益最大值

// TCP 传输状态
static volatile int client_connected = 0;
static int server_fd = -1;
static int client_fd = -1;
static pthread_t tcp_thread_id;

// 子系统通信状态
static subsys_handle_t subsys_handle = NULL; // 子系统句柄
static subsys_device_info_t device_info;     // 设备状态信息
static struct timeval last_subsys_update;    // 最后更新时间
// static pthread_mutex_t subsys_access_mutex = PTHREAD_MUTEX_INITIALIZER; // 已移除：libsubsys内部已有线程安全保护

// 自动控制状态
static bool auto_control_running = false;            // 自动控制是否正在运行
static pthread_t auto_control_thread_id;             // 自动控制线程ID
static volatile int auto_control_thread_running = 0; // 自动控制线程运行状态

// 媒体会话
static media_session_t *media_session = NULL;

// LVGL 对象
static lv_obj_t *img_canvas = NULL;
// 底部状态标签已禁用
// static lv_obj_t* status_label = NULL;
static lv_obj_t *info_label = NULL;
// static lv_obj_t* tcp_label = NULL;
static lv_obj_t *time_label = NULL;           // 时间显示标签
static lv_obj_t *subsys_panel = NULL;         // 子系统状态面板
static lv_obj_t *laser_status_label = NULL;   // 激光状态标签
static lv_obj_t *pump_status_label = NULL;    // 气泵状态标签
static lv_obj_t *heater1_status_label = NULL; // 加热器1状态标签
static lv_obj_t *heater2_status_label = NULL; // 加热器2状态标签
static lv_obj_t *separator1_label = NULL;     // 分隔符1
static lv_obj_t *separator2_label = NULL;     // 分隔符2
static lv_obj_t *separator3_label = NULL;     // 分隔符3
static lv_obj_t *menu_panel = NULL;           // 设置菜单面板
static lv_obj_t *menu_tcp_btn = NULL;         // TCP 按钮
static lv_obj_t *menu_display_btn = NULL;     // DISPLAY 按钮
static lv_obj_t *menu_exposure_btn = NULL;    // EXPOSURE 按钮
static lv_obj_t *menu_gain_btn = NULL;        // GAIN 按钮
static lv_obj_t *menu_usb_config_btn = NULL;  // USB CONFIG 按钮
static lv_obj_t *menu_heater1_btn = NULL;     // HEATER1 模式按钮
static lv_obj_t *menu_heater2_btn = NULL;     // HEATER2 模式按钮
static lv_obj_t *menu_pump_btn = NULL;        // PUMP 模式按钮
static lv_obj_t *menu_laser_btn = NULL;       // LASER 模式按钮
static lv_obj_t *menu_list_container = NULL;  // 菜单滚动容器
// static lv_obj_t* menu_close_btn = NULL;  // 关闭按钮
static lv_obj_t *subsys_status_label = NULL;  // 子系统状态标签 (新增)

// 帧率统计
static uint32_t frame_count = 0;
static struct timeval last_fps_time;
static float current_fps = 0.0f;

// 线程同步
static pthread_mutex_t frame_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t frame_ready = PTHREAD_COND_INITIALIZER;

// 当前帧数据
static media_frame_t current_frame = {0};
static int frame_available = 0;

// 曝光和增益控制
static int32_t exposure_value = 0; // 曝光值
static int32_t gain_value = 0;     // 增益值
static int exposure_step = 16;     // 曝光调整步长
static int gain_step = 32;         // 增益调整步长

// 温度滑动窗口滤波
#define TEMP_FILTER_WINDOW_SIZE 5  // 滑动窗口大小
typedef struct {
    float values[TEMP_FILTER_WINDOW_SIZE];  // 温度值数组
    int index;                              // 当前索引
    int count;                              // 已存储的值数量
    float average;                          // 当前平均值
    bool initialized;                       // 是否已初始化
} temp_filter_t;

static temp_filter_t temp1_filter = {0};   // 温度1滤波器
static temp_filter_t temp2_filter = {0};   // 温度2滤波器

typedef enum
{
    DEVICE_MODE_AUTO = 0,
    DEVICE_MODE_ON,
    DEVICE_MODE_OFF,
    DEVICE_MODE_COUNT
} device_mode_t;

typedef enum
{
    DEVICE_CTRL_HEATER1 = 0,
    DEVICE_CTRL_HEATER2,
    DEVICE_CTRL_PUMP,
    DEVICE_CTRL_LASER,
    DEVICE_CTRL_COUNT
} device_control_t;

static volatile device_mode_t device_modes[DEVICE_CTRL_COUNT] = {
    DEVICE_MODE_AUTO,
    DEVICE_MODE_AUTO,
    DEVICE_MODE_AUTO,
    DEVICE_MODE_AUTO
};

// 配置管理
static mxcamera_config_t current_config; // 当前配置
static int config_loaded = 0;            // 配置是否已加载

// ============================================================================
// 工具函数
// ============================================================================

/**
 * @brief 信号处理函数
 */
void signal_handler(int sig)
{
    static int signal_count = 0;
    signal_count++;
    
    printf("\nReceived signal %d (count: %d), cleaning up...\n", sig, signal_count);
    
    // 第二次信号强制退出
    if (signal_count >= 2)
    {
        printf("Force exit requested, terminating immediately...\n");
        fflush(stdout);
        _exit(1);
    }
    
    exit_flag = 1;
    tcp_enabled = 0; // 停止TCP传输

    // 保存当前配置到文件
    current_config.exposure = current_exposure;
    current_config.gain = current_gain;
    current_config.camera_width = camera_width;
    current_config.camera_height = camera_height;
    current_config.crop_top = crop_top;
    current_config.crop_left = crop_left;
    current_config.exposure_step = exposure_step;
    current_config.gain_step = gain_step;

    if (save_config_file(&current_config) == 0)
    {
        printf("Configuration saved on exit\n");
    }
    else
    {
        printf("Warning: Failed to save configuration on exit\n");
    }

    // 强制刷新输出缓冲区
    fflush(stdout);
    fflush(stderr);

    // 关闭TCP连接
    if (client_connected && client_fd >= 0)
    {
        shutdown(client_fd, SHUT_RDWR);
        close(client_fd);
        client_connected = 0;
        client_fd = -1;
    }
    if (server_fd >= 0)
    {
        shutdown(server_fd, SHUT_RDWR);
        close(server_fd);
        server_fd = -1;
    }

    // 通知所有等待线程
    pthread_mutex_lock(&frame_mutex);
    pthread_cond_broadcast(&frame_ready);
    pthread_mutex_unlock(&frame_mutex);

    // 给线程一些时间来响应退出标志
    usleep(100000); // 100ms
}

// ============================================================================
// 子系统通信函数
// ============================================================================

// 函数原型声明
void *auto_control_thread(void *arg);
void start_auto_control_mode(void);
void stop_auto_control_mode(void);

// 子系统状态显示函数声明
void show_subsys_on_status(void);
void show_subsys_off_status(void);
void show_subsys_off_status_user(void);
void hide_subsys_status_delayed(void);

// 温度滤波函数声明
void init_temp_filter(temp_filter_t *filter);
float update_temp_filter(temp_filter_t *filter, float new_value);
float get_filtered_temp1(void);
float get_filtered_temp2(void);
int get_device_info_with_filter(subsys_handle_t handle, subsys_device_info_t *info);

// ============================================================================
// 温度滤波函数实现
// ============================================================================

/**
 * @brief 初始化温度滤波器
 * @param filter 滤波器指针
 */
void init_temp_filter(temp_filter_t *filter)
{
    if (!filter) return;
    
    memset(filter->values, 0, sizeof(filter->values));
    filter->index = 0;
    filter->count = 0;
    filter->average = 0.0f;
    filter->initialized = false;
}

/**
 * @brief 更新温度滤波器并返回滤波后的值
 * @param filter 滤波器指针
 * @param new_value 新的温度值
 * @return 滤波后的温度值
 */
float update_temp_filter(temp_filter_t *filter, float new_value)
{
    if (!filter) return new_value;
    
    // 检查温度值是否合理 (0-200°C范围)
    if (new_value < 0.0f || new_value > 200.0f) {
        // 如果温度值异常，返回当前平均值（如果已初始化）
        if (filter->initialized) {
            printf("警告: 检测到异常温度值 %.1f°C，使用滤波值 %.1f°C\n", (double)new_value, (double)(filter->average));
            return filter->average;
        } else {
            // 如果还未初始化，使用一个合理的默认值
            printf("警告: 检测到异常温度值 %.1f°C，使用默认值 25.0°C\n", (double)new_value);
            return 25.0f;
        }
    }
    
    // 添加新值到滑动窗口
    filter->values[filter->index] = new_value;
    filter->index = (filter->index + 1) % TEMP_FILTER_WINDOW_SIZE;
    
    if (filter->count < TEMP_FILTER_WINDOW_SIZE) {
        filter->count++;
    }
    
    // 计算平均值
    float sum = 0.0f;
    for (int i = 0; i < filter->count; i++) {
        sum += filter->values[i];
    }
    filter->average = sum / filter->count;
    filter->initialized = true;
    
    return filter->average;
}

/**
 * @brief 获取滤波后的温度1值
 * @return 滤波后的温度值
 */
float get_filtered_temp1(void)
{
    if (temp1_filter.initialized) {
        return temp1_filter.average;
    } else {
        return device_info.temp1; // 如果滤波器未初始化，返回原始值
    }
}

/**
 * @brief 获取滤波后的温度2值
 * @return 滤波后的温度值
 */
float get_filtered_temp2(void)
{
    if (temp2_filter.initialized) {
        return temp2_filter.average;
    } else {
        return device_info.temp2; // 如果滤波器未初始化，返回原始值
    }
}

/**
 * @brief 获取设备信息并更新温度滤波器
 * @param handle 子系统句柄
 * @param info 设备信息结构指针
 * @return 0=成功，<0=失败
 */
int get_device_info_with_filter(subsys_handle_t handle, subsys_device_info_t *info)
{
    int result = subsys_get_device_info(handle, info);
    if (result == 0) {
        // 更新温度滤波器
        if (info->temp1_valid) {
            float filtered_temp1 = update_temp_filter(&temp1_filter, info->temp1);
            info->temp1 = filtered_temp1; // 用滤波后的值替换原始值
        }
        if (info->temp2_valid) {
            float filtered_temp2 = update_temp_filter(&temp2_filter, info->temp2);
            info->temp2 = filtered_temp2; // 用滤波后的值替换原始值
        }
    }
    return result;
}

// ============================================================================
// 设备模式控制工具函数
// ============================================================================

static const char *device_mode_to_string(device_mode_t mode)
{
    switch (mode)
    {
    case DEVICE_MODE_ON:
        return "ON";
    case DEVICE_MODE_OFF:
        return "OFF";
    case DEVICE_MODE_AUTO:
    default:
        return "AUTO";
    }
}

static const char *device_control_name(device_control_t device)
{
    switch (device)
    {
    case DEVICE_CTRL_HEATER1:
        return "Heater1";
    case DEVICE_CTRL_HEATER2:
        return "Heater2";
    case DEVICE_CTRL_PUMP:
        return "Pump";
    case DEVICE_CTRL_LASER:
        return "Laser";
    default:
        return "Unknown";
    }
}

static subsys_device_t device_control_to_subsys(device_control_t device)
{
    switch (device)
    {
    case DEVICE_CTRL_HEATER1:
        return SUBSYS_DEVICE_HEATER1;
    case DEVICE_CTRL_HEATER2:
        return SUBSYS_DEVICE_HEATER2;
    case DEVICE_CTRL_PUMP:
        return SUBSYS_DEVICE_PUMP;
    case DEVICE_CTRL_LASER:
        return SUBSYS_DEVICE_LASER;
    default:
        return SUBSYS_DEVICE_PUMP;
    }
}

static subsys_device_status_t *device_status_field(device_control_t device)
{
    switch (device)
    {
    case DEVICE_CTRL_HEATER1:
        return &device_info.heater1_status;
    case DEVICE_CTRL_HEATER2:
        return &device_info.heater2_status;
    case DEVICE_CTRL_PUMP:
        return &device_info.pump_status;
    case DEVICE_CTRL_LASER:
        return &device_info.laser_status;
    default:
        return NULL;
    }
}

static void apply_device_mode(device_control_t device)
{
    if (!subsys_handle)
    {
        return;
    }

    device_mode_t mode = device_modes[device];
    if (mode == DEVICE_MODE_AUTO)
    {
        return;
    }

    bool turn_on = (mode == DEVICE_MODE_ON);
    subsys_device_t subsys_device = device_control_to_subsys(device);
    subsys_device_status_t *status = device_status_field(device);

    if (status)
    {
        if (turn_on && *status == SUBSYS_STATUS_ON)
        {
            return;
        }
        if (!turn_on && *status == SUBSYS_STATUS_OFF)
        {
            return;
        }
    }

    int result = subsys_control_device(subsys_handle, subsys_device, turn_on);
    if (result != 0)
    {
        const char *error_info = subsys_get_last_error(subsys_handle);
        printf("Warning: Failed to set %s to %s (%s)\n",
               device_control_name(device),
               device_mode_to_string(mode),
               error_info ? error_info : "unknown error");
    }
    else
    {
        printf("Manual override applied: %s -> %s\n",
               device_control_name(device),
               device_mode_to_string(mode));
    }
}

static void set_device_mode(device_control_t device, device_mode_t mode)
{
    device_mode_t previous = device_modes[device];
    if (previous == mode)
    {
        if (mode != DEVICE_MODE_AUTO)
        {
            apply_device_mode(device);
        }
        return;
    }

    device_modes[device] = mode;
    printf("Device mode updated: %s -> %s\n",
           device_control_name(device),
           device_mode_to_string(mode));

    if (mode != DEVICE_MODE_AUTO)
    {
        apply_device_mode(device);
    }
}

static void cycle_device_mode(device_control_t device)
{
    device_mode_t next_mode = (device_mode_t)((device_modes[device] + 1) % DEVICE_MODE_COUNT);
    set_device_mode(device, next_mode);
}

static void enforce_manual_device_modes(void)
{
    if (!subsys_handle)
    {
        return;
    }

    for (int i = 0; i < DEVICE_CTRL_COUNT; ++i)
    {
        if (device_modes[i] != DEVICE_MODE_AUTO)
        {
            apply_device_mode((device_control_t)i);
        }
    }
}

/**
 * @brief 初始化子系统通信
 * @return 0=成功，-1=失败
 */
int init_subsystem(void)
{
    printf("初始化子系统通信...\n");

    // 初始化温度滤波器
    init_temp_filter(&temp1_filter);
    init_temp_filter(&temp2_filter);
    printf("温度滑动窗口滤波器已初始化（窗口大小: %d）\n", TEMP_FILTER_WINDOW_SIZE);

    // 检查串口设备是否存在
    if (access("/dev/ttyS4", F_OK) != 0)
    {
        printf("警告: 串口设备 /dev/ttyS4 不存在，将以离线模式运行\n");
        goto offline_mode;
    }

    // 初始化子系统句柄（带超时保护）
    printf("正在连接子系统（最多等待3秒）...\n");
    subsys_handle = subsys_init(NULL, 0); // 使用默认设备和波特率
    if (!subsys_handle)
    {
        printf("警告: 子系统初始化失败，可能的原因：\n");
        printf("  - 串口设备被占用\n");
        printf("  - 硬件连接问题\n");
        printf("  - 权限不足\n");
        printf("将以离线模式运行\n");
        goto offline_mode;
    }

    // 增强子系统通信配置 - 更高的重试次数和延迟
    printf("配置子系统通信参数...\n");
    subsys_set_max_retry_times(subsys_handle, 3);  // 增加到8次重试
    subsys_set_retry_delay(subsys_handle, 20);    // 100ms重试延迟
    printf("已设置子系统重试次数: 8次，重试延迟: 100ms\n");

    // 检查子系统版本（带超时）
    printf("检查子系统版本...\n");
    subsys_version_t version;

    if (subsys_get_version(subsys_handle, &version) == 0)
    {
        printf("子系统版本: %s\n", version.version_string);

        // 手动解析版本字符串 "Firmware: v0.2.0-e6eab60, Built: 2025-08-19 17:27:58, Branch: ES2.0"
        char *version_pos = strstr(version.version_string, "v");
        if (version_pos != NULL)
        {
            int major = 0, minor = 0, patch = 0;
            if (sscanf(version_pos, "v%d.%d.%d", &major, &minor, &patch) == 3)
            {
                printf("解析到版本号: %d.%d.%d\n", major, minor, patch);

                // 手动设置解析出的版本号
                version.major = major;
                version.minor = minor;
                version.patch = patch;

                // 检查版本是否满足要求（>= 0.2.0）
                if (major > 0 || (major == 0 && minor > 2) || (major == 0 && minor == 2 && patch >= 0))
                {
                    printf("子系统版本检查通过: %d.%d.%d >= 0.2.0\n", major, minor, patch);
                }
                else
                {
                    printf("警告: 子系统版本过低，要求 >= 0.2.0，当前 %d.%d.%d\n", major, minor, patch);
                    printf("继续运行，但可能功能受限\n");
                }
            }
            else
            {
                printf("版本号解析失败，但检测到版本字符串，继续运行\n");
            }
        }
        else
        {
            printf("错误: 获取子系统版本失败\n");
            printf("错误: 多次尝试后仍无法与子系统通信，可能原因：\n");
            printf("  - 子系统硬件故障\n");
            printf("  - 串口波特率不匹配\n");
            printf("  - 子系统固件问题\n");
            printf("关闭子系统连接，将以离线模式运行\n");
            subsys_cleanup(subsys_handle);
            subsys_handle = NULL;
            goto offline_mode;
        }
    }
    else
    {
        goto offline_mode;
    }
    // 等待一段时间后再查询MCU序列号，避免命令冲突
    usleep(200000); // 等待200ms

    // 获取MCU序列号（进一步验证通信稳定性，带重试机制）
    char serial[64];

    printf("尝试获取MCU序列号...\n");
    int result = subsys_get_mcu_serial(subsys_handle, serial, sizeof(serial));
    if (result == 0)
    {
        printf("MCU序列号: %s\n", serial);
    }
    else
    {
        printf("警告: 获取MCU序列号失败，错误代码: %d\n", result);
        goto offline_mode;
    }

    // 初始化设备状态
    memset(&device_info, 0, sizeof(device_info));
    device_info.pump_status = SUBSYS_STATUS_OFF;
    device_info.laser_status = SUBSYS_STATUS_OFF;
    device_info.heater1_status = SUBSYS_STATUS_OFF;
    device_info.heater2_status = SUBSYS_STATUS_OFF;

    // 初始化时间戳
    gettimeofday(&last_subsys_update, NULL);

    result = subsys_reset_all_devices(subsys_handle);
    if (result != 0)
    {
        printf("错误: 重置所有设备失败，错误代码: %d\n", result);
        goto offline_mode;
    }

    printf("子系统通信初始化完成\n");
    return 0;

offline_mode:
    // 离线模式：初始化设备状态为未知状态
    memset(&device_info, 0, sizeof(device_info));
    device_info.pump_status = SUBSYS_STATUS_UNKNOWN;
    device_info.laser_status = SUBSYS_STATUS_UNKNOWN;
    device_info.heater1_status = SUBSYS_STATUS_UNKNOWN;
    device_info.heater2_status = SUBSYS_STATUS_UNKNOWN;
    device_info.temp1_valid = false;
    device_info.temp2_valid = false;

    subsys_handle = NULL;

    return -2; // 不返回错误，继续启动
}

/**
 * @brief 自动控制线程 - 气泵持续运行，激光间隔1.5秒开启1.5秒
 */
void *auto_control_thread(void *arg)
{
    (void)arg;
    int result = 0;
    bool error_exit = false; // 标志是否因错误退出

    printf("自动控制线程已启动（包含设备监控功能）\n");

    // 等待一小段时间，确保主线程完成初始化
    usleep(100000); // 100ms

    // 检查线程运行状态
    if (!auto_control_thread_running || exit_flag)
    {
        printf("自动控制线程：收到退出信号，立即退出\n");
        return NULL;
    }

    enforce_manual_device_modes();

    // 启动加热片1
    if (subsys_handle)
    {
        if (device_modes[DEVICE_CTRL_HEATER1] == DEVICE_MODE_AUTO)
        {
            result = subsys_control_device(subsys_handle, SUBSYS_DEVICE_HEATER1, true);
            if (result == 0)
            {
                printf("自动控制：加热片1已启动（持续运行）\n");
            }
            else
            {
                const char *error_info = subsys_get_last_error(subsys_handle);
                printf("错误: 加热片1启动失败，错误信息: %s\n", error_info ? error_info : "未知错误");

                if (error_info && strstr(error_info, "RSP_FAIL_ALREADY_RUNNING"))
                {
                    if (get_device_info_with_filter(subsys_handle, &device_info) == 0 &&
                        device_info.heater1_status == SUBSYS_STATUS_ON)
                    {
                        printf("自动控制：加热片1已在运行中，继续执行\n");
                        gettimeofday(&last_subsys_update, NULL);
                        goto HEATER1_RUNNING; // 跳转到加热片1运行逻辑
                    }
                }

                error_exit = true;
                goto EXIT;
            }

HEATER1_RUNNING:
            for (int i = 0; i < 30; i++)
            {
                if (device_modes[DEVICE_CTRL_HEATER1] != DEVICE_MODE_AUTO)
                {
                    apply_device_mode(DEVICE_CTRL_HEATER1);
                    break;
                }

                if (get_device_info_with_filter(subsys_handle, &device_info) == 0)
                {
                    gettimeofday(&last_subsys_update, NULL);
                }

                if (!auto_control_thread_running && exit_flag)
                {
                    usleep(200000); // 0.2秒
                    goto EXIT;
                }
                else
                {
                    sleep(1);
                }

                if (device_modes[DEVICE_CTRL_HEATER1] != DEVICE_MODE_AUTO)
                {
                    apply_device_mode(DEVICE_CTRL_HEATER1);
                    break;
                }

                if ((double)(device_info.temp1) > 30.0 && device_info.heater1_status == SUBSYS_STATUS_ON)
                {
                    printf("警告: 加热片1温度过高（%.1f°C），正在降低功率\n", (double)(device_info.temp1));
                    subsys_control_device(subsys_handle, SUBSYS_DEVICE_HEATER1, false);
                    break;
                }
            }
        }
        else
        {
            apply_device_mode(DEVICE_CTRL_HEATER1);
        }
    }

    // 启动加热片2
    if (subsys_handle)
    {
        if (device_modes[DEVICE_CTRL_HEATER2] == DEVICE_MODE_AUTO)
        {
            result = subsys_control_device(subsys_handle, SUBSYS_DEVICE_HEATER2, true);
            if (result == 0)
            {
                printf("自动控制：加热片2已启动（持续运行）\n");
            }
            else
            {
                const char *error_info = subsys_get_last_error(subsys_handle);
                printf("错误: 加热片2启动失败，错误信息: %s\n", error_info ? error_info : "未知错误");

                if (error_info && strstr(error_info, "RSP_FAIL_ALREADY_RUNNING"))
                {
                    if (get_device_info_with_filter(subsys_handle, &device_info) == 0 &&
                        device_info.heater2_status == SUBSYS_STATUS_ON)
                    {
                        printf("自动控制：加热片2已在运行中，继续执行\n");
                        gettimeofday(&last_subsys_update, NULL);
                        goto HEATER2_RUNNING; // 跳转到加热片2运行逻辑
                    }
                }

                error_exit = true;
                goto EXIT;
            }

HEATER2_RUNNING:
            printf("等待50秒，以加热 HEATER2 至60摄氏度");
            for (int i = 0; i < 50; i++)
            {
                if (device_modes[DEVICE_CTRL_HEATER2] != DEVICE_MODE_AUTO)
                {
                    apply_device_mode(DEVICE_CTRL_HEATER2);
                    break;
                }

                if (get_device_info_with_filter(subsys_handle, &device_info) == 0)
                {
                    gettimeofday(&last_subsys_update, NULL);
                }

                if (!auto_control_thread_running && exit_flag)
                {
                    usleep(200000); // 0.2秒
                    goto EXIT;
                }
                else
                {
                    sleep(1);
                }

                if (device_modes[DEVICE_CTRL_HEATER2] != DEVICE_MODE_AUTO)
                {
                    apply_device_mode(DEVICE_CTRL_HEATER2);
                    break;
                }

                if ((double)(device_info.temp2) > 60.0 && device_info.heater2_status == SUBSYS_STATUS_ON)
                {
                    printf("警告: 加热片2温度过高（%.1f°C），正在降低功率\n", (double)(device_info.temp2));
                    subsys_control_device(subsys_handle, SUBSYS_DEVICE_HEATER2, false);
                    break;
                }
            }
        }
        else
        {
            apply_device_mode(DEVICE_CTRL_HEATER2);
        }
    }

    while (auto_control_thread_running && !exit_flag)
    {
        enforce_manual_device_modes();

        if (!subsys_handle)
        {
            printf("警告: 子系统不可用，等待重连...\n");
            sleep(2);
            continue;
        }

        bool pump_auto = (device_modes[DEVICE_CTRL_PUMP] == DEVICE_MODE_AUTO);
        bool pump_already_running = false;

        if (pump_auto)
        {
            result = subsys_control_device(subsys_handle, SUBSYS_DEVICE_PUMP, true);
            if (result == 0)
            {
                printf("自动控制：气泵已启动（持续运行）\n");
            }
            else
            {
                const char *error_info = subsys_get_last_error(subsys_handle);
                printf("错误: 气泵启动失败，错误信息: %s\n", error_info ? error_info : "未知错误");

                if (error_info && strstr(error_info, "RSP_FAIL_ALREADY_RUNNING"))
                {
                    if (get_device_info_with_filter(subsys_handle, &device_info) == 0 &&
                        device_info.pump_status == SUBSYS_STATUS_ON)
                    {
                        printf("自动控制：气泵已在运行中，继续执行\n");
                        gettimeofday(&last_subsys_update, NULL);
                        pump_already_running = true;
                    }
                }

                if (!pump_already_running)
                {
                    error_exit = true;
                    goto EXIT;
                }
            }
        }
        else
        {
            apply_device_mode(DEVICE_CTRL_PUMP);
        }

        if (get_device_info_with_filter(subsys_handle, &device_info) == 0)
        {
            gettimeofday(&last_subsys_update, NULL);
        }

        printf("自动控制：激光关闭，等待1.5秒并监控设备状态...\n");

        for (int i = 0; i < 3 && auto_control_thread_running && !exit_flag; i++)
        {
            usleep(500000);

            pump_auto = (device_modes[DEVICE_CTRL_PUMP] == DEVICE_MODE_AUTO);
            if (!pump_auto)
            {
                apply_device_mode(DEVICE_CTRL_PUMP);
            }

            if (get_device_info_with_filter(subsys_handle, &device_info) == 0)
            {
                gettimeofday(&last_subsys_update, NULL);
            }
        }

        if (!auto_control_thread_running || exit_flag)
        {
            break;
        }

        bool laser_auto = (device_modes[DEVICE_CTRL_LASER] == DEVICE_MODE_AUTO);
        if (laser_auto)
        {
            result = subsys_control_device(subsys_handle, SUBSYS_DEVICE_LASER, true);
            if (result == 0)
            {
                printf("自动控制：激光开启\n");
            }
            else
            {
                printf("警告: 激光开启失败\n");
            }
        }
        else
        {
            apply_device_mode(DEVICE_CTRL_LASER);
        }

        for (int i = 0; i < 3 && auto_control_thread_running && !exit_flag; i++)
        {
            usleep(500000);

            pump_auto = (device_modes[DEVICE_CTRL_PUMP] == DEVICE_MODE_AUTO);
            if (!pump_auto)
            {
                apply_device_mode(DEVICE_CTRL_PUMP);
            }

            laser_auto = (device_modes[DEVICE_CTRL_LASER] == DEVICE_MODE_AUTO);
            if (!laser_auto)
            {
                apply_device_mode(DEVICE_CTRL_LASER);
            }

            if (get_device_info_with_filter(subsys_handle, &device_info) == 0)
            {
                gettimeofday(&last_subsys_update, NULL);
            }
        }

        laser_auto = (device_modes[DEVICE_CTRL_LASER] == DEVICE_MODE_AUTO);
        if (laser_auto)
        {
            result = subsys_control_device(subsys_handle, SUBSYS_DEVICE_LASER, false);
            if (result == 0)
            {
                printf("自动控制：激光关闭\n");
            }
            else
            {
                printf("警告: 激光关闭失败\n");
            }
        }
        else
        {
            apply_device_mode(DEVICE_CTRL_LASER);
        }

        if (device_modes[DEVICE_CTRL_HEATER2] == DEVICE_MODE_AUTO)
        {
            if ((double)(device_info.temp2) > 60.0 && device_info.heater2_status == SUBSYS_STATUS_ON)
            {
                printf("警告: 加热片2温度过高（%.1f°C），正在降低功率\n", (double)(device_info.temp2));
                subsys_control_device(subsys_handle, SUBSYS_DEVICE_HEATER2, false);
            }
            else if ((double)(device_info.temp2) < 55.0 && device_info.heater2_status == SUBSYS_STATUS_OFF)
            {
                printf("警告: 加热片2温度过低（%.1f°C），正在提高功率\n", (double)(device_info.temp2));
                subsys_control_device(subsys_handle, SUBSYS_DEVICE_HEATER2, true);
            }
        }
        else
        {
            apply_device_mode(DEVICE_CTRL_HEATER2);
        }

        if (device_modes[DEVICE_CTRL_HEATER1] == DEVICE_MODE_AUTO)
        {
            if ((double)(device_info.temp1) > 35.0 && device_info.heater1_status == SUBSYS_STATUS_ON)
            {
                printf("警告: 加热片1温度过高（%.1f°C），正在降低功率\n", (double)(device_info.temp1));
                subsys_control_device(subsys_handle, SUBSYS_DEVICE_HEATER1, false);
            }
            else if ((double)(device_info.temp1) < 28.0 && device_info.heater1_status == SUBSYS_STATUS_OFF)
            {
                printf("警告: 加热片1温度过低（%.1f°C），正在提高功率\n", (double)(device_info.temp1));
                subsys_control_device(subsys_handle, SUBSYS_DEVICE_HEATER1, true);
            }
        }
        else
        {
            apply_device_mode(DEVICE_CTRL_HEATER1);
        }
    }

EXIT:

    // 清理：关闭所有设备
    if (subsys_handle)
    {
        printf("自动控制：正在关闭所有设备...\n");

        // 在退出阶段先获取从设备状态
        if (get_device_info_with_filter(subsys_handle, &device_info) == 0)
        {
            gettimeofday(&last_subsys_update, NULL);
        }

        // 根据对应设备状态逐个关闭（仅限自动模式），手动模式保持用户设置
        if (device_modes[DEVICE_CTRL_HEATER1] == DEVICE_MODE_AUTO)
        {
            if (device_info.heater1_status == SUBSYS_STATUS_ON)
            {
                result = subsys_control_device(subsys_handle, SUBSYS_DEVICE_HEATER1, false);
                if (result != 0)
                {
                    const char *error_info = subsys_get_last_error(subsys_handle);
                    printf("警告: 关闭加热片1失败，错误信息: %s\n", error_info ? error_info : "未知错误");
                }
            }
        }
        else
        {
            apply_device_mode(DEVICE_CTRL_HEATER1);
        }

        if (device_modes[DEVICE_CTRL_HEATER2] == DEVICE_MODE_AUTO)
        {
            if (device_info.heater2_status == SUBSYS_STATUS_ON)
            {
                result = subsys_control_device(subsys_handle, SUBSYS_DEVICE_HEATER2, false);
                if (result != 0)
                {
                    const char *error_info = subsys_get_last_error(subsys_handle);
                    printf("警告: 关闭加热片2失败，错误信息: %s\n", error_info ? error_info : "未知错误");
                }
            }
        }
        else
        {
            apply_device_mode(DEVICE_CTRL_HEATER2);
        }

        if (device_modes[DEVICE_CTRL_LASER] == DEVICE_MODE_AUTO)
        {
            if (device_info.laser_status == SUBSYS_STATUS_ON)
            {
                result = subsys_control_device(subsys_handle, SUBSYS_DEVICE_LASER, false);
                if (result != 0)
                {
                    const char *error_info = subsys_get_last_error(subsys_handle);
                    printf("警告: 关闭激光失败，错误信息: %s\n", error_info ? error_info : "未知错误");
                }
            }
        }
        else
        {
            apply_device_mode(DEVICE_CTRL_LASER);
        }

        if (device_modes[DEVICE_CTRL_PUMP] == DEVICE_MODE_AUTO)
        {
            if (device_info.pump_status == SUBSYS_STATUS_ON)
            {
                result = subsys_control_device(subsys_handle, SUBSYS_DEVICE_PUMP, false);
                if (result != 0)
                {
                    const char *error_info = subsys_get_last_error(subsys_handle);
                    printf("警告: 关闭气泵失败，错误信息: %s\n", error_info ? error_info : "未知错误");
                }
            }
        }
        else
        {
            apply_device_mode(DEVICE_CTRL_PUMP);
        }

        auto_control_thread_running = 0;
        auto_control_running = false;

        // 最后再更新一遍从设备状态
        if (get_device_info_with_filter(subsys_handle, &device_info) == 0)
        {
            gettimeofday(&last_subsys_update, NULL);
        }

        printf("自动控制：所有设备已关闭\n");
    }

    // 根据退出原因显示相应状态
    if (error_exit)
    {
        // 错误退出时显示红色状态
        show_subsys_off_status();
        printf("自动控制线程因错误退出\n");
    }
    else
    {
        // 正常退出时不显示状态（由stop_auto_control_mode函数显示绿色状态）
        printf("自动控制线程正常退出\n");
    }

    printf("自动控制线程已退出\n");
    return NULL;
}

/**
 * @brief 启动自动控制模式（手动调用）
 */
void start_auto_control_mode(void)
{
    if (!subsys_handle)
    {
        printf("无法启动自动控制：子系统不可用\n");
        return;
    }

    if (auto_control_running)
    {
        printf("自动控制已在运行中\n");
        return;
    }

    // 显示子系统开启状态
    show_subsys_on_status();

    auto_control_thread_running = 1;
    auto_control_running = true;

    // 创建自动控制线程 (低优先级)
    pthread_attr_t auto_attr;
    struct sched_param auto_param;
    
    pthread_attr_init(&auto_attr);
    pthread_attr_setdetachstate(&auto_attr, PTHREAD_CREATE_JOINABLE);
    
    // 设置调度策略为SCHED_OTHER，最低优先级
    pthread_attr_setschedpolicy(&auto_attr, SCHED_OTHER);
    auto_param.sched_priority = 0;  // SCHED_OTHER的最低优先级
    pthread_attr_setschedparam(&auto_attr, &auto_param);
    pthread_attr_setinheritsched(&auto_attr, PTHREAD_EXPLICIT_SCHED);
    
    printf("Setting auto control thread priority to: %d (SCHED_OTHER)\n", auto_param.sched_priority);
    
    if (pthread_create(&auto_control_thread_id, &auto_attr, auto_control_thread, NULL) != 0)
    {
        printf("错误: 创建自动控制线程失败\n");
        auto_control_thread_running = 0;
        auto_control_running = false;
        
        // 创建失败时显示关闭状态
        show_subsys_off_status();
        
        pthread_attr_destroy(&auto_attr);
        return;
    }
    
    pthread_attr_destroy(&auto_attr);
}

/**
 * @brief 停止自动控制模式
 */
void stop_auto_control_mode(void)
{
    if (!auto_control_running)
    {
        printf("自动控制未运行\n");
        return;
    }

    printf("停止自动控制模式...\n");

    auto_control_thread_running = 0;

    // 等待线程结束
    pthread_join(auto_control_thread_id, NULL);

    auto_control_running = false;

    // 显示子系统关闭状态（用户主动退出 - 绿色）
    show_subsys_off_status_user();

    printf("自动控制模式已停止\n");
}

/**
 * @brief 清理子系统资源
 */
void cleanup_subsystem(void)
{
    printf("清理子系统资源...\n");

    // 停止自动控制线程
    if (auto_control_running)
    {
        stop_auto_control_mode();
    }

    // 如果子系统可用，执行清理操作
    if (subsys_handle)
    {
        // 停止温度控制
        subsys_stop_temp_control(subsys_handle, 1);
        subsys_stop_temp_control(subsys_handle, 2);

        // 关闭所有设备
        subsys_control_device(subsys_handle, SUBSYS_DEVICE_PUMP, false);
        subsys_control_device(subsys_handle, SUBSYS_DEVICE_LASER, false);
        subsys_control_device(subsys_handle, SUBSYS_DEVICE_HEATER1, false);
        subsys_control_device(subsys_handle, SUBSYS_DEVICE_HEATER2, false);

        // 释放子系统句柄
        subsys_cleanup(subsys_handle);
    }

    subsys_handle = NULL;

    printf("子系统资源清理完成\n");
}

/**
 * @brief 更新子系统状态显示
 */
void update_subsys_status_display(void)
{
    // 检查所有标签是否已创建
    if (!laser_status_label || !pump_status_label || !heater1_status_label || !heater2_status_label)
    {
        return;
    }

    // 如果子系统不可用，所有显示为灰色
    if (!subsys_handle)
    {
        const uint8_t r = 64, g = 64, b = 64;
        lv_obj_set_style_text_color(laser_status_label, lv_color_make(r, g, b), 0);
        lv_obj_set_style_text_color(pump_status_label, lv_color_make(r, g, b), 0);
        lv_obj_set_style_text_color(heater1_status_label, lv_color_make(r, g, b), 0);
        lv_obj_set_style_text_color(heater2_status_label, lv_color_make(r, g, b), 0);

        // 分隔符也设置为灰色
        lv_obj_set_style_text_color(separator1_label, lv_color_make(r, g, b), 0);
        lv_obj_set_style_text_color(separator2_label, lv_color_make(r, g, b), 0);
        lv_obj_set_style_text_color(separator3_label, lv_color_make(r, g, b), 0);

        lv_label_set_text(laser_status_label, "L");
        lv_label_set_text(pump_status_label, "P");
        lv_label_set_text(heater1_status_label, "H1:离线");
        lv_label_set_text(heater2_status_label, "H2:离线");
        return;
    }

    // 通信正常时，分隔符恢复为白色
    lv_obj_set_style_text_color(separator1_label, lv_color_white(), 0);
    lv_obj_set_style_text_color(separator2_label, lv_color_white(), 0);
    lv_obj_set_style_text_color(separator3_label, lv_color_white(), 0);


    
    // 激光器状态（红色=开启，白色=关闭）
    if (device_info.laser_status == SUBSYS_STATUS_ON)
    {
        lv_obj_set_style_text_color(laser_status_label, lv_color_make(255, 0, 0), 0);
    }
    else
    {
        lv_obj_set_style_text_color(laser_status_label, lv_color_white(), 0);
    }
    lv_label_set_text(laser_status_label, "L");

    // 气泵状态（红色=开启，白色=关闭）
    if (device_info.pump_status == SUBSYS_STATUS_ON)
    {
        lv_obj_set_style_text_color(pump_status_label, lv_color_make(255, 0, 0), 0);
    }
    else
    {
        lv_obj_set_style_text_color(pump_status_label, lv_color_white(), 0);
    }
    lv_label_set_text(pump_status_label, "P");

    // 加热器1状态和温度
    char heater1_text[32];
    if (device_info.temp1_valid)
    {
        snprintf(heater1_text, sizeof(heater1_text), "H1:%.2f°C", (double)device_info.temp1);
    }
    else
    {
        snprintf(heater1_text, sizeof(heater1_text), "H1:--°C");
    }

    if (device_info.heater1_status == SUBSYS_STATUS_ON)
    {
        lv_obj_set_style_text_color(heater1_status_label, lv_color_make(255, 0, 0), 0);
    }
    else
    {
        lv_obj_set_style_text_color(heater1_status_label, lv_color_white(), 0);
    }
    lv_label_set_text(heater1_status_label, heater1_text);

    // 加热器2状态和温度
    char heater2_text[32];
    if (device_info.temp2_valid)
    {
        snprintf(heater2_text, sizeof(heater2_text), "H2:%.2f°C", (double)device_info.temp2);
    }
    else
    {
        snprintf(heater2_text, sizeof(heater2_text), "H2:--°C");
    }

    if (device_info.heater2_status == SUBSYS_STATUS_ON)
    {
        lv_obj_set_style_text_color(heater2_status_label, lv_color_make(255, 0, 0), 0);
    }
    else
    {
        lv_obj_set_style_text_color(heater2_status_label, lv_color_white(), 0);
    }
    lv_label_set_text(heater2_status_label, heater2_text);
}

/**
 * @brief 显示子系统开启状态
 */
void show_subsys_on_status(void)
{
    if (!subsys_status_label || !screen_on)
        return;

    lv_label_set_text(subsys_status_label, "SUBSYS ON");
    lv_obj_set_style_text_color(subsys_status_label, lv_color_make(0, 255, 0), 0);  // 绿色字体
    lv_obj_set_style_bg_color(subsys_status_label, lv_color_make(0, 40, 0), 0);     // 深绿背景
    lv_obj_set_style_border_color(subsys_status_label, lv_color_make(0, 255, 0), 0); // 绿色边框
    lv_obj_clear_flag(subsys_status_label, LV_OBJ_FLAG_HIDDEN);
    
    printf("GUI: Showing SUBSYS ON status\n");
}

/**
 * @brief 显示子系统关闭状态（错误退出 - 红色）
 */
void show_subsys_off_status(void)
{
    if (!subsys_status_label || !screen_on)
        return;

    lv_label_set_text(subsys_status_label, "SUBSYS OFF");
    lv_obj_set_style_text_color(subsys_status_label, lv_color_make(255, 0, 0), 0);  // 红色字体
    lv_obj_set_style_bg_color(subsys_status_label, lv_color_make(40, 0, 0), 0);     // 深红背景
    lv_obj_set_style_border_color(subsys_status_label, lv_color_make(255, 0, 0), 0); // 红色边框
    lv_obj_clear_flag(subsys_status_label, LV_OBJ_FLAG_HIDDEN);
    
    printf("GUI: Showing SUBSYS OFF status (error exit - red)\n");
}

/**
 * @brief 显示子系统关闭状态（用户主动退出 - 绿色）
 */
void show_subsys_off_status_user(void)
{
    if (!subsys_status_label || !screen_on)
        return;

    lv_label_set_text(subsys_status_label, "SUBSYS OFF");
    lv_obj_set_style_text_color(subsys_status_label, lv_color_make(0, 255, 0), 0);  // 绿色字体
    lv_obj_set_style_bg_color(subsys_status_label, lv_color_make(0, 40, 0), 0);     // 深绿背景
    lv_obj_set_style_border_color(subsys_status_label, lv_color_make(0, 255, 0), 0); // 绿色边框
    lv_obj_clear_flag(subsys_status_label, LV_OBJ_FLAG_HIDDEN);
    
    printf("GUI: Showing SUBSYS OFF status (user exit - green)\n");
}

/**
 * @brief 隐藏子系统状态显示 (延迟隐藏)
 */
void hide_subsys_status_delayed(void)
{
    if (!subsys_status_label)
        return;
    
    // 使用简单的延时隐藏机制
    static struct timeval show_time;
    static int status_shown = 0;
    
    if (!status_shown) {
        gettimeofday(&show_time, NULL);
        status_shown = 1;
        return;
    }
    
    struct timeval current_time;
    gettimeofday(&current_time, NULL);
    long time_diff = (current_time.tv_sec - show_time.tv_sec) * 1000000 +
                     (current_time.tv_usec - show_time.tv_usec);
    
    // 2秒后隐藏
    if (time_diff >= 2000000) {
        lv_obj_add_flag(subsys_status_label, LV_OBJ_FLAG_HIDDEN);
        status_shown = 0;
        printf("GUI: Hiding SUBSYS status after 2 seconds\n");
    }
}

/**
 * @brief 打印程序使用方法
 */
void print_usage(const char *program_name)
{
    printf("Usage: %s [OPTIONS]\n", program_name);
    printf("\nOptions:\n");
    printf("  --width WIDTH      Set camera width (default: %d)\n", DEFAULT_CAMERA_WIDTH);
    printf("  --height HEIGHT    Set camera height (default: %d)\n", DEFAULT_CAMERA_HEIGHT);
    printf("  --enable-tcp       Enable TCP transmission on startup\n");
    printf("  --tcp-port PORT    Set TCP server port (default: %d)\n", DEFAULT_PORT);
    printf("  --tcp-ip IP        Set TCP server IP (default: %s)\n", DEFAULT_SERVER_IP);
    printf("  --help, -h         Show this help message\n");
    printf("\nExamples:\n");
    printf("  %s --width 1920 --height 1080\n", program_name);
    printf("  %s --tcp-port 9999 --tcp-ip 192.168.1.100\n", program_name);
    printf("\nSupported resolutions (depends on camera):\n");
    printf("  1920x1080 (Full HD)\n");
    printf("  1600x1200 (4:3)\n");
    printf("  1280x720 (HD)\n");
    printf("  640x480 (VGA)\n");
    printf("\nControls:\n");
    printf("  当菜单隐藏时:\n");
    printf("  KEY_MENU (PIN %d)  - 显示设置菜单\n", KEY_MENU_PIN);
    printf("  KEY_OK (PIN %d)    - 拍照\n", KEY_OK_PIN);
    printf("  KEY_X (PIN %d)     - 短按: 切换自动控制 / 长按: 关机\n", KEY_X_PIN);
    printf("\n  当菜单显示时:\n");
    printf("  KEY_UP/RIGHT       - 菜单导航上/增加数值(调整模式)\n");
    printf("  KEY_DOWN/LEFT      - 菜单导航下/减少数值(调整模式)\n");
    printf("  KEY_OK             - 确认选择\n");
    printf("  KEY_MENU           - 隐藏菜单\n");
    printf("  Ctrl+C - Exit\n");
}

/**
 * @brief 解析命令行参数
 * @param argc 参数个数
 * @param argv 参数数组
 * @return 0 成功，-1 失败，1 显示帮助后退出
 */
int parse_arguments(int argc, char *argv[])
{
    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--width") == 0)
        {
            if (i + 1 >= argc)
            {
                printf("Error: --width requires a value\n");
                return -1;
            }
            camera_width = atoi(argv[++i]);
            if (camera_width <= 0 || camera_width > 4096)
            {
                printf("Error: Invalid width %d (must be 1-4096)\n", camera_width);
                return -1;
            }
        }
        else if (strcmp(argv[i], "--height") == 0)
        {
            if (i + 1 >= argc)
            {
                printf("Error: --height requires a value\n");
                return -1;
            }
            camera_height = atoi(argv[++i]);
            if (camera_height <= 0 || camera_height > 4096)
            {
                printf("Error: Invalid height %d (must be 1-4096)\n", camera_height);
                return -1;
            }
        }
        else if (strcmp(argv[i], "--enable-tcp") == 0)
        {
            tcp_enabled = 1;
            printf("TCP transmission enabled via command line\n");
            
            // 检查当前USB模式是否已经是RNDIS
            usb_mode_t current_mode = get_usb_mode();
            if (current_mode == USB_MODE_RNDIS)
            {
                printf("USB mode is already RNDIS, no need to switch\n");
            }
            else
            {
                printf("Current USB mode: %s\n", get_usb_mode_name(current_mode));
                printf("Automatically switching USB mode to RNDIS for TCP transmission...\n");
                
                // 自动切换USB模式到RNDIS
                if (set_usb_mode(USB_MODE_RNDIS) != 0)
                {
                    printf("Warning: Failed to switch USB mode to RNDIS\n");
                    printf("TCP transmission may not work properly without RNDIS mode\n");
                }
                else
                {
                    printf("USB mode switched to RNDIS successfully\n");
                    // 等待USB模式切换完成
                    printf("Waiting for USB configuration to take effect...\n");
                    sleep(3); // 等待3秒让USB重新配置
                }
            }
        }
        else if (strcmp(argv[i], "--tcp-port") == 0)
        {
            if (i + 1 >= argc)
            {
                printf("Error: --tcp-port requires a value\n");
                return -1;
            }
            int port = atoi(argv[++i]);
            if (port <= 0 || port > 65535)
            {
                printf("Error: Invalid port %d (must be 1-65535)\n", port);
                return -1;
            }
            // Note: We'll need to modify DEFAULT_PORT usage later
            printf("TCP port set to: %d\n", port);
        }
        else if (strcmp(argv[i], "--tcp-ip") == 0)
        {
            if (i + 1 >= argc)
            {
                printf("Error: --tcp-ip requires a value\n");
                return -1;
            }
            // Note: We'll need to modify DEFAULT_SERVER_IP usage later
            printf("TCP IP set to: %s\n", argv[++i]);
        }
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
        {
            print_usage(argv[0]);
            return 1;
        }
        else
        {
            printf("Error: Unknown option '%s'\n", argv[i]);
            printf("Use '%s --help' for usage information.\n", argv[0]);
            return -1;
        }
    }

    printf("Camera configuration:\n");
    printf("  Resolution: %dx%d\n", camera_width, camera_height);
    printf("  Format: SBGGR10 (RAW10)\n");

    return 0;
}

/**
 * @brief 检查和配置显示设备
 */
void check_display_config(void)
{
    printf("=== Display Configuration Check ===\n");

    // 检查帧缓冲设备
    printf("Framebuffer device info:\n");
    system("ls -la /dev/fb* 2>/dev/null || echo 'No framebuffer devices found'");

    // 获取当前帧缓冲配置
    printf("Current framebuffer settings:\n");
    system("fbset 2>/dev/null || echo 'fbset not available'");

    // 检查显示相关的设备文件
    printf("Display-related devices:\n");
    system("ls -la /sys/class/graphics/ 2>/dev/null || echo 'No graphics devices found'");

    printf("=== End Display Check ===\n");
}

/**
 * @brief 获取高精度时间戳
 */
uint64_t get_time_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/**
 * @brief 创建TCP服务器
 */
int create_server(int port)
{
    int fd;
    struct sockaddr_in addr;
    int opt = 1;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
    {
        perror("socket failed");
        return -1;
    }

    // 设置 SO_REUSEADDR 选项，允许重用地址
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
    {
        perror("setsockopt SO_REUSEADDR failed");
        close(fd);
        return -1;
    }

    // 设置 SO_REUSEPORT 选项（如果支持），允许重用端口
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) < 0)
    {
        // 某些系统可能不支持 SO_REUSEPORT，这里只打印警告
        printf("Warning: SO_REUSEPORT not supported\n");
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr(DEFAULT_SERVER_IP);
    addr.sin_port = htons(port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        perror("bind failed");
        close(fd);
        return -1;
    }

    if (listen(fd, 1) < 0)
    {
        perror("listen failed");
        close(fd);
        return -1;
    }

    printf("TCP Server listening on %s:%d\n", DEFAULT_SERVER_IP, port);
    return fd;
}

/**
 * @brief 发送图像帧数据到客户端
 */
int send_frame(int fd, void *data, size_t size, uint32_t frame_id, uint64_t timestamp)
{
    // 首先发送帧同步标识
    const char *frame_sync = "---MIXOSENSE---FRAME---";
    size_t sync_len = strlen(frame_sync);
    
    if (send(fd, frame_sync, sync_len, MSG_NOSIGNAL) != (ssize_t)sync_len)
    {
        return -1;
    }

    struct frame_header header = {
        .magic = 0xDEADBEEF,
        .frame_id = frame_id,
        .width = camera_width,
        .height = camera_height,
        .pixfmt = CAMERA_PIXELFORMAT,
        .size = size,
        .timestamp = timestamp,
        .reserved = {0, 0}};

    // 发送帧头
    if (send(fd, &header, sizeof(header), MSG_NOSIGNAL) != sizeof(header))
    {
        return -1;
    }

    // 分块发送数据
    size_t sent = 0;
    uint8_t *ptr = (uint8_t *)data;

    while (sent < size && !exit_flag)
    {
        size_t to_send = (size - sent) > CHUNK_SIZE ? CHUNK_SIZE : (size - sent);
        ssize_t result = send(fd, ptr + sent, to_send, MSG_NOSIGNAL);

        if (result <= 0)
        {
            return -1;
        }

        sent += result;
    }

    return 0;
}

/**
 * @brief TCP数据发送线程函数
 */
void *tcp_sender_thread(void *arg)
{
    (void)arg; // 避免未使用参数警告
    printf("TCP sender thread started\n");
    static uint32_t tcp_frame_counter = 0;

    while (!exit_flag && tcp_enabled)
    {
        // 等待客户端连接
        if (!client_connected && tcp_enabled && server_fd >= 0)
        {
            struct sockaddr_in client_addr;
            socklen_t client_len = sizeof(client_addr);

            printf("Waiting for TCP client connection...\n");

            // 设置 socket 为非阻塞模式，避免 accept 无限阻塞
            fd_set readfds;
            struct timeval timeout;

            FD_ZERO(&readfds);
            FD_SET(server_fd, &readfds);
            timeout.tv_sec = 1; // 1秒超时
            timeout.tv_usec = 0;

            int select_result = select(server_fd + 1, &readfds, NULL, NULL, &timeout);

            if (select_result > 0 && FD_ISSET(server_fd, &readfds))
            {
                client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);

                if (client_fd >= 0)
                {
                    printf("TCP Client connected from %s\n", inet_ntoa(client_addr.sin_addr));
                    
                    // 设置更大的发送缓冲区以减少网络阻塞
                    int send_buffer_size = 2 * 1024 * 1024; // 2MB 发送缓冲区
                    if (setsockopt(client_fd, SOL_SOCKET, SO_SNDBUF, &send_buffer_size, sizeof(send_buffer_size)) < 0) {
                        perror("Warning: Failed to set send buffer size");
                    }
                    
                    // 设置TCP_NODELAY以减少延迟
                    int flag = 1;
                    if (setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag)) < 0) {
                        perror("Warning: Failed to set TCP_NODELAY");
                    }
                    
                    client_connected = 1;
                    
                    // TCP连接建立时自动关闭屏幕以减少系统负载
                    if (screen_on) {
                        printf("TCP connection established, turning off screen to optimize transmission\n");
                        turn_screen_off();
                    }
                }
                else
                {
                    if (tcp_enabled && !exit_flag)
                    {
                        perror("accept failed");
                    }
                }
            }
            else if (select_result < 0 && tcp_enabled && !exit_flag)
            {
                perror("select failed");
                break;
            }

            // 检查是否需要退出
            if (!tcp_enabled || exit_flag)
            {
                break;
            }
            continue;
        }

        // 等待新帧数据
        pthread_mutex_lock(&frame_mutex);
        while (current_frame.data == NULL && !exit_flag && tcp_enabled)
        {
            struct timespec timeout;
            clock_gettime(CLOCK_REALTIME, &timeout);
            timeout.tv_sec += 1; // 1秒超时
            pthread_cond_timedwait(&frame_ready, &frame_mutex, &timeout);
        }

        if (current_frame.data && !exit_flag && tcp_enabled && client_connected)
        {
            // 发送原始RAW10帧数据
            uint64_t timestamp = get_time_ns();
            if (send_frame(client_fd, current_frame.data, current_frame.size,
                           tcp_frame_counter++, timestamp) < 0)
            {
                printf("TCP Client disconnected (frame %d)\n", tcp_frame_counter);
                close(client_fd);
                client_connected = 0;
                
                // TCP连接断开时恢复屏幕显示
                printf("TCP connection lost, restoring screen display\n");
                turn_screen_on();
            }
        }

        pthread_mutex_unlock(&frame_mutex);

        // 如果TCP被禁用，退出循环
        if (!tcp_enabled)
        {
            break;
        }

        usleep(1000); // 减少到100微秒以提高传输效率
    }

    // 清理TCP连接
    if (client_connected && client_fd >= 0)
    {
        shutdown(client_fd, SHUT_RDWR);
        close(client_fd);
        client_connected = 0;
        client_fd = -1;
        
        // TCP线程结束时恢复屏幕显示
        printf("TCP sender thread ending, restoring screen display\n");
        turn_screen_on();
    }

    printf("TCP sender thread terminated\n");
    return NULL;
}

/**
 * @brief 清理动态分配的图像缓冲区
 */
void cleanup_image_buffers(void)
{
    // 这个函数会在 update_image_display 中通过静态变量引用
    // 实际的清理在程序退出时自动发生
    printf("Image buffers cleanup initiated\n");
}

/**
 * @brief 更新最后活动时间
 */
void update_activity_time(void)
{
    gettimeofday(&last_activity_time, NULL);
}

/**
 * @brief 关闭屏幕
 */
void turn_screen_off(void)
{
    if (!screen_on)
        return;

    printf("Turning screen OFF (auto-sleep after 5s pause)\n");
    screen_on = 0;

    // 设置屏幕亮度为0或关闭背光
    system("echo 0 > /sys/class/backlight/*/brightness 2>/dev/null");

    // 隐藏所有UI元素
    if (img_canvas)
        lv_obj_add_flag(img_canvas, LV_OBJ_FLAG_HIDDEN);
    if (info_label)
        lv_obj_add_flag(info_label, LV_OBJ_FLAG_HIDDEN);
    if (time_label)
        lv_obj_add_flag(time_label, LV_OBJ_FLAG_HIDDEN);
    if (subsys_panel)
        lv_obj_add_flag(subsys_panel, LV_OBJ_FLAG_HIDDEN);
    if (subsys_status_label)  // 新增：隐藏子系统状态标签
        lv_obj_add_flag(subsys_status_label, LV_OBJ_FLAG_HIDDEN);
    if (menu_panel)
    {
        lv_obj_add_flag(menu_panel, LV_OBJ_FLAG_HIDDEN);
        menu_visible = 0; // 重置菜单状态
    }

    // 清空屏幕为黑色
    lv_obj_t *scr = lv_disp_get_scr_act(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
}

/**
 * @brief 打开屏幕
 */
void turn_screen_on(void)
{
    if (screen_on)
        return;

    printf("Turning screen ON (key wake-up)\n");
    screen_on = 1;

    // 恢复屏幕亮度
    system("echo 255 > /sys/class/backlight/*/brightness 2>/dev/null");

    // 显示所有UI元素
    if (img_canvas)
        lv_obj_clear_flag(img_canvas, LV_OBJ_FLAG_HIDDEN);
    if (info_label)
        lv_obj_clear_flag(info_label, LV_OBJ_FLAG_HIDDEN);
    if (time_label)
        lv_obj_clear_flag(time_label, LV_OBJ_FLAG_HIDDEN);
    // 注意：菜单面板保持隐藏状态，不自动显示

    // 更新活动时间
    update_activity_time();
}

/**
 * @brief 检查屏幕超时并自动关闭
 */
void check_screen_timeout(void)
{
    // 有TCP连接时不自动关闭屏幕，或者只在屏幕开启且显示关闭时检查超时
    if (!screen_on || display_enabled || client_connected)
        return;

    struct timeval current_time;
    gettimeofday(&current_time, NULL);

    long time_since_activity = (current_time.tv_sec - last_activity_time.tv_sec) * 1000000 +
                               (current_time.tv_usec - last_activity_time.tv_usec);

    // 5秒超时 (5,000,000 微秒)
    if (time_since_activity >= 5000000)
    {
        turn_screen_off();
    }
}

/**
 * @brief 计算缩放后的图像尺寸 (保持宽高比，宽度对齐屏幕)
 * @param src_width 源图像宽度
 * @param src_height 源图像高度
 * @param dst_width 输出缩放后宽度
 * @param dst_height 输出缩放后高度
 */
void calculate_scaled_size(int src_width, int src_height, int *dst_width, int *dst_height)
{
    if (src_width <= 0 || src_height <= 0)
    {
        *dst_width = DISPLAY_WIDTH;
        *dst_height = DISPLAY_HEIGHT;
        return;
    }

    // 计算宽高比
    float aspect_ratio = (float)src_height / (float)src_width;

    // 宽度固定为屏幕宽度，高度根据宽高比计算
    *dst_width = DISPLAY_WIDTH; // 强制使用像素宽度
    *dst_height = (int)(DISPLAY_WIDTH * aspect_ratio);

    // 确保高度不超过屏幕高度 DISPLAY_HEIGHT
    if (*dst_height > DISPLAY_HEIGHT)
    {
        *dst_height = DISPLAY_HEIGHT;
        *dst_width = (int)(DISPLAY_HEIGHT / aspect_ratio);
    }

    // 确保最小尺寸
    if (*dst_width < FBTFT_LCD_DEFAULT_WIDTH / 2)
        *dst_width = FBTFT_LCD_DEFAULT_WIDTH / 2;
    if (*dst_height < FBTFT_LCD_DEFAULT_HEIGHT / 2)
        *dst_height = FBTFT_LCD_DEFAULT_HEIGHT / 2;

    // printf("Image scaling: %dx%d -> %dx%d (aspect ratio: %.3f)\n",
    //        src_width, src_height, *dst_width, *dst_height, (double)aspect_ratio);
}

/**
 * @brief 计算帧率
 */
void update_fps(void)
{
    struct timeval current_time;
    gettimeofday(&current_time, NULL);

    long time_diff = (current_time.tv_sec - last_fps_time.tv_sec) * 1000000 +
                     (current_time.tv_usec - last_fps_time.tv_usec);

    if (time_diff >= FPS_UPDATE_INTERVAL)
    {
        current_fps = (float)frame_count * 1000000.0f / (float)time_diff;
        frame_count = 0;
        last_fps_time = current_time;

        // FPS 现在由 update_system_info 函数统一更新到 info_label
    }
}

/**
 * @brief 获取CPU使用率
 * @return CPU使用率百分比 (0.0-100.0)
 */
float get_cpu_usage(void)
{
    static unsigned long long last_total = 0, last_idle = 0;
    unsigned long long total, idle, user, nice, system, iowait, irq, softirq, steal;

    FILE *fp = fopen("/proc/stat", "r");
    if (!fp)
    {
        return -1.0f;
    }

    if (fscanf(fp, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
               &user, &nice, &system, &idle, &iowait, &irq, &softirq, &steal) != 8)
    {
        fclose(fp);
        return -1.0f;
    }
    fclose(fp);

    total = user + nice + system + idle + iowait + irq + softirq + steal;

    if (last_total == 0)
    {
        // 第一次调用，保存当前值
        last_total = total;
        last_idle = idle;
        return 0.0f;
    }

    unsigned long long total_diff = total - last_total;
    unsigned long long idle_diff = idle - last_idle;

    float cpu_usage = 0.0f;
    if (total_diff > 0)
    {
        cpu_usage = 100.0f * (1.0f - (float)idle_diff / (float)total_diff);
    }

    last_total = total;
    last_idle = idle;

    return cpu_usage;
}

/**
 * @brief 获取内存使用率
 * @return 内存使用率百分比 (0.0-100.0)
 */
float get_memory_usage(void)
{
    FILE *fp = fopen("/proc/meminfo", "r");
    if (!fp)
    {
        return -1.0f;
    }

    unsigned long mem_total = 0, mem_free = 0, buffers = 0, cached = 0;
    char line[256];

    while (fgets(line, sizeof(line), fp))
    {
        if (sscanf(line, "MemTotal: %lu kB", &mem_total) == 1)
            continue;
        if (sscanf(line, "MemFree: %lu kB", &mem_free) == 1)
            continue;
        if (sscanf(line, "Buffers: %lu kB", &buffers) == 1)
            continue;
        if (sscanf(line, "Cached: %lu kB", &cached) == 1)
            continue;
    }
    fclose(fp);

    if (mem_total == 0)
    {
        return -1.0f;
    }

    unsigned long mem_used = mem_total - mem_free - buffers - cached;
    return 100.0f * (float)mem_used / (float)mem_total;
}

/**
 * @brief 更新系统信息显示（合并图像大小、帧率、CPU和内存占用）
 */
void update_system_info(void)
{
    static struct timeval last_update = {0};
    struct timeval current_time;
    gettimeofday(&current_time, NULL);

    // 每500ms更新一次系统信息（提高更新频率以显示实时FPS）
    long time_diff = (current_time.tv_sec - last_update.tv_sec) * 1000000 +
                     (current_time.tv_usec - last_update.tv_usec);

    if (time_diff >= 500000)
    { // 0.5秒
        float cpu_usage = get_cpu_usage();
        float mem_usage = get_memory_usage();

        // 只有在屏幕开启且info_label存在时才更新UI
        if (info_label && screen_on) {
            char info_text[64];
            // 格式：采集帧率 CPU占用率% 内存占用率%
            // 例如：30.4cFPS 98% 70% (c表示capture采集帧率)
            snprintf(info_text, sizeof(info_text),
                     "%.1fcFPS  %.0f%%  %.0f%%",
                     (double)current_fps,
                     (double)(cpu_usage >= 0 ? cpu_usage : 0),
                     (double)(mem_usage >= 0 ? mem_usage : 0));

            lv_label_set_text(info_label, info_text);
        }
        
        last_update = current_time;
    }
}

/**
 * @brief SBGGR10格式数据解包（标量版本）- 参考v4l2_bench实现
 * @param raw_bytes 5字节的RAW10数据（包含4个像素）
 * @param pixels 输出的4个16位像素值
 */
void unpack_sbggr10_scalar(const uint8_t raw_bytes[5], uint16_t pixels[4])
{
    // 重构40位数据
    uint64_t combined = ((uint64_t)raw_bytes[4] << 32) |
                        ((uint64_t)raw_bytes[3] << 24) |
                        ((uint64_t)raw_bytes[2] << 16) |
                        ((uint64_t)raw_bytes[1] << 8) |
                        (uint64_t)raw_bytes[0];

    // 提取4个10位像素值（小端序，从低位开始）
    pixels[0] = (uint16_t)((combined >> 0) & 0x3FF);
    pixels[1] = (uint16_t)((combined >> 10) & 0x3FF);
    pixels[2] = (uint16_t)((combined >> 20) & 0x3FF);
    pixels[3] = (uint16_t)((combined >> 30) & 0x3FF);
}

/**
 * @brief SBGGR10图像数据完整解包函数
 * @param raw_data 输入的RAW10数据
 * @param raw_size RAW10数据大小（字节）
 * @param output_pixels 输出的16位像素数组
 * @param width 图像宽度
 * @param height 图像高度
 * @return 0成功，-1失败
 */
int unpack_sbggr10_image(const uint8_t *raw_data, size_t raw_size,
                         uint16_t *output_pixels, int width, int height)
{
    if (!raw_data || !output_pixels || raw_size == 0)
    {
        return -1;
    }

    // 验证数据大小（必须是5的倍数）
    if (raw_size % 5 != 0)
    {
        printf("Error: RAW data size (%zu) must be multiple of 5\n", raw_size);
        return -1;
    }

    size_t expected_pixels = width * height;
    size_t available_pixels = raw_size / 5 * 4;

    if (available_pixels < expected_pixels)
    {
        printf("Warning: Not enough RAW data (%zu pixels available, %zu expected)\n",
               available_pixels, expected_pixels);
    }

    // 解包RAW10数据
    size_t raw_pos = 0;
    size_t pixel_pos = 0;
    size_t max_pixels = (available_pixels < expected_pixels) ? available_pixels : expected_pixels;

    while (raw_pos + 5 <= raw_size && pixel_pos + 4 <= max_pixels)
    {
        uint16_t pixels[4];
        unpack_sbggr10_scalar(raw_data + raw_pos, pixels);

        // 复制像素数据，注意边界检查
        for (int i = 0; i < 4 && pixel_pos < max_pixels; i++)
        {
            output_pixels[pixel_pos++] = pixels[i];
        }

        raw_pos += 5;
    }

    // 填充剩余像素（如果有）
    while (pixel_pos < expected_pixels)
    {
        output_pixels[pixel_pos++] = 0;
    }

    return 0;
}

/**
 * @brief 16位像素数据缩放到目标尺寸
 * @param src_pixels 源16位像素数据
 * @param src_width 源图像宽度
 * @param src_height 源图像高度
 * @param dst_pixels 目标16位像素数据
 * @param dst_width 目标图像宽度
 * @param dst_height 目标图像高度
 */
void scale_pixels(const uint16_t *src_pixels, int src_width, int src_height,
                  uint16_t *dst_pixels, int dst_width, int dst_height)
{
    float x_ratio = (float)src_width / dst_width;
    float y_ratio = (float)src_height / dst_height;

    for (int y = 0; y < dst_height; y++)
    {
        for (int x = 0; x < dst_width; x++)
        {
            int src_x = (int)(x * x_ratio);
            int src_y = (int)(y * y_ratio);

            // 边界检查
            if (src_x >= src_width)
                src_x = src_width - 1;
            if (src_y >= src_height)
                src_y = src_height - 1;

            int src_idx = src_y * src_width + src_x;
            int dst_idx = y * dst_width + x;

            dst_pixels[dst_idx] = src_pixels[src_idx];
        }
    }
}

/**
 * @brief 16位像素转换为RGB565格式
 * @param pixels 输入的16位像素数据（10位有效值）
 * @param rgb565_data 输出的RGB565数据
 * @param width 图像宽度
 * @param height 图像高度
 */
void convert_pixels_to_rgb565(const uint16_t *pixels, uint16_t *rgb565_data,
                              int width, int height)
{
    int total_pixels = width * height;

    for (int i = 0; i < total_pixels; i++)
    {
        // 将10位值转换为8位灰度值
        uint8_t gray = (uint8_t)(pixels[i] >> 2);

        // 转换为 RGB565 (灰度图)
        uint16_t rgb565 = ((gray >> 3) << 11) | ((gray >> 2) << 5) | (gray >> 3);
        rgb565_data[i] = rgb565;
    }
}

/**
 * @brief 横屏图像适配函数 - 将图像缩放并居中到全屏缓冲区
 * @param src_buffer 源图像缓冲区
 * @param src_width 源图像宽度
 * @param src_height 源图像高度
 * @param dst_buffer 目标全屏缓冲区 (DISPLAY_WIDTH X DISPLAY_HEIGHT)
 * @return 0 成功，-1 失败
 */
int landscape_image_fit(const uint16_t *src_buffer, int src_width, int src_height,
                        uint16_t *dst_buffer)
{
    if (!src_buffer || !dst_buffer || src_width <= 0 || src_height <= 0)
    {
        return -1;
    }

    // 清空目标缓冲区 (黑色背景)
    memset(dst_buffer, 0, DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t));

    // 计算居中位置
    int x_offset = (DISPLAY_WIDTH - src_width) / 2;
    int y_offset = (DISPLAY_HEIGHT - src_height) / 2;

    // 确保不超出边界
    if (x_offset < 0)
        x_offset = 0;
    if (y_offset < 0)
        y_offset = 0;

    int copy_width = (src_width > DISPLAY_WIDTH) ? DISPLAY_WIDTH : src_width;
    int copy_height = (src_height > DISPLAY_HEIGHT) ? DISPLAY_HEIGHT : src_height;

    // 复制图像数据到居中位置
    for (int y = 0; y < copy_height; y++)
    {
        int dst_y = y + y_offset;
        if (dst_y >= DISPLAY_HEIGHT)
            break;

        for (int x = 0; x < copy_width; x++)
        {
            int dst_x = x + x_offset;
            if (dst_x >= DISPLAY_WIDTH)
                break;

            int src_idx = y * src_width + x;
            int dst_idx = dst_y * DISPLAY_WIDTH + dst_x;

            dst_buffer[dst_idx] = src_buffer[src_idx];
        }
    }

    return 0;
}

/**
 * @brief 更新图像显示 (使用正确的SBGGR10解包和缩放，优化性能)
 */
void update_image_display(void)
{
    // 使用非阻塞锁尝试，避免阻塞按键处理
    if (pthread_mutex_trylock(&frame_mutex) != 0)
    {
        return; // 如果无法获取锁，跳过本次更新
    }

    if (frame_available && current_frame.data && img_canvas)
    {
        // 计算动态缩放尺寸
        int scaled_width, scaled_height;
        calculate_scaled_size(current_frame.width, current_frame.height,
                              &scaled_width, &scaled_height);

        // 更新当前图像尺寸
        current_img_width = scaled_width;
        current_img_height = scaled_height;

        // 创建图像处理缓冲区 (动态分配，支持不同分辨率)
        static uint16_t *unpacked_buffer = NULL;                        // 原始尺寸解包缓冲区
        static uint16_t scaled_pixels[DISPLAY_WIDTH * DISPLAY_HEIGHT];  // 缩放后的像素缓冲区
        static uint16_t scaled_rgb565[DISPLAY_WIDTH * DISPLAY_HEIGHT];  // RGB565缓冲区
        static uint16_t display_buffer[DISPLAY_WIDTH * DISPLAY_HEIGHT]; // 最终显示缓冲区
        static int last_processed_width = 0, last_processed_height = 0;
        static size_t unpacked_buffer_size = 0;

        // 动态分配解包缓冲区（根据当前摄像头分辨率）
        size_t required_size = camera_width * camera_height;
        if (unpacked_buffer == NULL || unpacked_buffer_size < required_size)
        {
            if (unpacked_buffer)
            {
                free(unpacked_buffer);
            }
            unpacked_buffer = malloc(required_size * sizeof(uint16_t));
            if (!unpacked_buffer)
            {
                printf("Error: Failed to allocate unpacked buffer (%zu bytes)\n",
                       required_size * sizeof(uint16_t));
                pthread_mutex_unlock(&frame_mutex);
                return;
            }
            unpacked_buffer_size = required_size;
            printf("Allocated unpacked buffer: %dx%d (%zu pixels)\n",
                   camera_width, camera_height, required_size);
        }

        // 只在尺寸变化时打印处理信息，减少日志开销
        if (current_frame.width != last_processed_width || current_frame.height != last_processed_height)
        {
            printf("Processing frame: %dx%d -> %dx%d\n",
                   current_frame.width, current_frame.height, scaled_width, scaled_height);
            last_processed_width = current_frame.width;
            last_processed_height = current_frame.height;
        }

        // 第一步：SBGGR10 解包到原始尺寸的16位像素数据
        if (unpack_sbggr10_image((const uint8_t *)current_frame.data, current_frame.size,
                                 unpacked_buffer, current_frame.width, current_frame.height) != 0)
        {
            printf("Error: Failed to unpack SBGGR10 data\n");
            pthread_mutex_unlock(&frame_mutex);
            return;
        }

        // 第二步：缩放到目标尺寸
        scale_pixels(unpacked_buffer, current_frame.width, current_frame.height,
                     scaled_pixels, scaled_width, scaled_height);

        // 第三步：转换为RGB565格式
        convert_pixels_to_rgb565(scaled_pixels, scaled_rgb565, scaled_width, scaled_height);

        // 第四步：横屏适配 (居中显示到全屏缓冲区)
        if (landscape_image_fit(scaled_rgb565, scaled_width, scaled_height, display_buffer) == 0)
        {
            // 创建 LVGL 图像描述符
            static lv_img_dsc_t img_dsc;
            img_dsc.header.always_zero = 0;
            img_dsc.header.w = DISPLAY_WIDTH;  // 使用全屏宽度
            img_dsc.header.h = DISPLAY_HEIGHT; // 使用全屏高度
            img_dsc.data_size = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
            img_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
            img_dsc.data = (uint8_t *)display_buffer;

            // 更新图像 (使用全屏显示)
            lv_img_set_src(img_canvas, &img_dsc);
            lv_obj_set_size(img_canvas, DISPLAY_WIDTH, DISPLAY_HEIGHT);
            lv_obj_set_pos(img_canvas, 0, 0); // 左上角对齐

            // 只在尺寸变化时打印成功信息
            if (current_frame.width != last_processed_width || current_frame.height != last_processed_height)
            {
                printf("Image updated: %dx%d -> %dx%d (SBGGR10 properly unpacked)\n",
                       current_frame.width, current_frame.height, scaled_width, scaled_height);
            }
        }
        else
        {
            printf("Error: Failed to fit image for landscape display\n");
        }

        frame_available = 0;
    }

    pthread_mutex_unlock(&frame_mutex);
}

// ============================================================================
// 摄像头采集线程
// ============================================================================

/**
 * @brief 摄像头采集线程函数 (始终运行，不受显示状态影响)
 */
void *camera_thread(void *arg)
{
    printf("Camera thread started (always running)\n");
    
    // 确认当前线程的调度策略和优先级
    int policy;
    struct sched_param param;
    pthread_getschedparam(pthread_self(), &policy, &param);
    printf("Camera thread running with policy: %s, priority: %d\n",
           (policy == SCHED_FIFO) ? "SCHED_FIFO" : 
           (policy == SCHED_RR) ? "SCHED_RR" : "SCHED_OTHER",
           param.sched_priority);
    
    // 设置线程取消状态
    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
    pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);

    while (!exit_flag)
    {
        // 添加取消点
        pthread_testcancel();
        
        media_frame_t frame;

        // 采集帧数据 (进一步优化超时时间以达到30FPS)
        int ret = libmedia_session_capture_frame(media_session, &frame, 25); // 25ms 超时，更积极的帧率目标
        
        // 在每次操作后检查取消
        pthread_testcancel();
        
        if (ret == 0)
        {
            // 再次检查退出标志
            if (exit_flag)
            {
                libmedia_session_release_frame(media_session, &frame);
                break;
            }

            pthread_mutex_lock(&frame_mutex);

            // 更新当前帧
            if (current_frame.data)
            {
                libmedia_session_release_frame(media_session, &current_frame);
            }

            current_frame = frame;
            frame_available = 1;
            frame_count++;

            // 通知显示更新和TCP发送线程
            pthread_cond_broadcast(&frame_ready);
            pthread_mutex_unlock(&frame_mutex);

            // 更新采集帧率统计
            update_fps();
        }
        else if (ret == -EAGAIN)
        {
            // 超时，继续循环检查退出标志
            continue;
        }
        else
        {
            if (!exit_flag)
            { // 只在未退出时打印错误
                printf("Failed to capture frame: %d\n", ret);
            }
            // 在错误情况下添加最小延迟，最大化帧率
            usleep(1000); // 1ms，最小错误恢复时间
        }
        
        // 在循环末尾再次检查退出标志，确保快速响应
        if (exit_flag)
        {
            break;
        }
        
        // 添加取消点
        pthread_testcancel();
    }

    printf("Camera thread exited\n");
    return NULL;
}

// ============================================================================
// 按键处理函数
// ============================================================================

/**
 * @brief 处理按键输入
 * 菜单隐藏时：OK=拍照，MENU=显示菜单，X=自动控制切换
 * 菜单显示时：OK=确认，UP/RIGHT=增加，DOWN/LEFT=减少，MENU=隐藏菜单
 */
void handle_keys(void)
{
    // 静态变量保存按键状态和去抖动计数 (移除POWER按键相关)
    static int last_key_up_state = 1;
    static int last_key_down_state = 1;
    static int last_key_left_state = 1;
    static int last_key_right_state = 1;
    static int last_key_menu_state = 1;
    static int last_key_ok_state = 1;
    static int last_key_x_state = 1;
    
    static int key_up_debounce_count = 0;
    static int key_down_debounce_count = 0;
    static int key_left_debounce_count = 0;
    static int key_right_debounce_count = 0;
    static int key_menu_debounce_count = 0;
    static int key_ok_debounce_count = 0;
    static int key_x_debounce_count = 0;
    
    static struct timeval last_key_check = {0};
    static struct timeval key_x_press_start = {0};
    static bool key_x_long_press_triggered = false;
    
    const int debounce_threshold = 1;

    // 限制按键检查频率
    struct timeval current_time;
    gettimeofday(&current_time, NULL);
    long time_since_last_check = (current_time.tv_sec - last_key_check.tv_sec) * 1000000 +
                                 (current_time.tv_usec - last_key_check.tv_usec);

    if (time_since_last_check < 1000)
    {
        return;
    }
    last_key_check = current_time;

    // 读取所有按键状态 (移除POWER按键)
    int current_key_up = GET_KEY_UP;
    int current_key_down = GET_KEY_DOWN;
    int current_key_left = GET_KEY_LEFT;
    int current_key_right = GET_KEY_RIGHT;
    int current_key_menu = GET_KEY_MENU;
    int current_key_ok = GET_KEY_OK;
    int current_key_x = GET_KEY_X;

    // 检查任意按键是否被按下（屏幕唤醒，移除POWER按键）
    int any_key_pressed = (current_key_up == 0) || (current_key_down == 0) ||
                          (current_key_left == 0) || (current_key_right == 0) ||
                          (current_key_menu == 0) || (current_key_ok == 0) ||
                          (current_key_x == 0);

    if (any_key_pressed && !screen_on)
    {
        turn_screen_on();
        return; // 屏幕唤醒后跳过其他按键处理，避免误操作
    }

    // 屏幕开启时才处理按键功能
    if (!screen_on)
        return;

    // ========== KEY_UP (方向键上) 处理 ==========
    if (current_key_up == last_key_up_state)
    {
        key_up_debounce_count = 0;
    }
    else
    {
        key_up_debounce_count++;
        if (key_up_debounce_count >= debounce_threshold)
        {
            if (last_key_up_state == 1 && current_key_up == 0)
            {
                if (menu_visible)
                {
                    if (in_adjustment_mode)
                    {
                        // 在调整模式下，UP增加数值
                        if (adjustment_type == 0) // 曝光调整
                        {
                            adjust_exposure_up();
                            printf("KEY_UP pressed - Increase exposure\n");
                        }
                        else if (adjustment_type == 1) // 增益调整
                        {
                            adjust_gain_up();
                            printf("KEY_UP pressed - Increase gain\n");
                        }
                    }
                    else
                    {
                        menu_navigate_up();
                        printf("KEY_UP pressed - Menu navigate up\n");
                    }
                }
                update_activity_time();
            }
            last_key_up_state = current_key_up;
            key_up_debounce_count = 0;
        }
    }

    // ========== KEY_DOWN (方向键下) 处理 ==========
    if (current_key_down == last_key_down_state)
    {
        key_down_debounce_count = 0;
    }
    else
    {
        key_down_debounce_count++;
        if (key_down_debounce_count >= debounce_threshold)
        {
            if (last_key_down_state == 1 && current_key_down == 0)
            {
                if (menu_visible)
                {
                    if (in_adjustment_mode)
                    {
                        // 在调整模式下，DOWN减少数值
                        if (adjustment_type == 0) // 曝光调整
                        {
                            adjust_exposure_down();
                            printf("KEY_DOWN pressed - Decrease exposure\n");
                        }
                        else if (adjustment_type == 1) // 增益调整
                        {
                            adjust_gain_down();
                            printf("KEY_DOWN pressed - Decrease gain\n");
                        }
                    }
                    else
                    {
                        menu_navigate_down();
                        printf("KEY_DOWN pressed - Menu navigate down\n");
                    }
                }
                update_activity_time();
            }
            last_key_down_state = current_key_down;
            key_down_debounce_count = 0;
        }
    }

    // ========== KEY_LEFT (方向键左) 处理 ==========
    if (current_key_left == last_key_left_state)
    {
        key_left_debounce_count = 0;
    }
    else
    {
        key_left_debounce_count++;
        if (key_left_debounce_count >= debounce_threshold)
        {
            if (last_key_left_state == 1 && current_key_left == 0)
            {
                if (menu_visible)
                {
                    if (in_adjustment_mode)
                    {
                        // 在调整模式下，LEFT减少数值
                        if (adjustment_type == 0) // 曝光调整
                        {
                            adjust_exposure_down();
                            printf("KEY_LEFT pressed - Decrease exposure\n");
                        }
                        else if (adjustment_type == 1) // 增益调整
                        {
                            adjust_gain_down();
                            printf("KEY_LEFT pressed - Decrease gain\n");
                        }
                    }
                    // else
                    // {
                    //     // 在菜单中，LEFT可以用作取消或退出
                    //     hide_settings_menu();
                    //     printf("KEY_LEFT pressed - Hide menu\n");
                    // }
                }
                update_activity_time();
            }
            last_key_left_state = current_key_left;
            key_left_debounce_count = 0;
        }
    }

    // ========== KEY_RIGHT (方向键右) 处理 ==========
    if (current_key_right == last_key_right_state)
    {
        key_right_debounce_count = 0;
    }
    else
    {
        key_right_debounce_count++;
        if (key_right_debounce_count >= debounce_threshold)
        {
            if (last_key_right_state == 1 && current_key_right == 0)
            {
                if (menu_visible)
                {
                    if (in_adjustment_mode)
                    {
                        // 在调整模式下，RIGHT增加数值
                        if (adjustment_type == 0) // 曝光调整
                        {
                            adjust_exposure_up();
                            printf("KEY_RIGHT pressed - Increase exposure\n");
                        }
                        else if (adjustment_type == 1) // 增益调整
                        {
                            adjust_gain_up();
                            printf("KEY_RIGHT pressed - Increase gain\n");
                        }
                    }
                    else
                    {
                        // 在菜单中，RIGHT也可以作为确认（与OK相同）
                        menu_confirm_selection();
                        printf("KEY_RIGHT pressed - Menu confirm\n");
                    }
                }
                update_activity_time();
            }
            last_key_right_state = current_key_right;
            key_right_debounce_count = 0;
        }
    }

    // ========== KEY_MENU (菜单键) 处理 ==========
    if (current_key_menu == last_key_menu_state)
    {
        key_menu_debounce_count = 0;
    }
    else
    {
        key_menu_debounce_count++;
        if (key_menu_debounce_count >= debounce_threshold)
        {
            if (last_key_menu_state == 1 && current_key_menu == 0)
            {
                if (in_adjustment_mode)
                {
                    // 退出调整模式
                    in_adjustment_mode = 0;
                    printf("KEY_MENU pressed - Exit adjustment mode\n");
                }
                else if (menu_visible)
                {
                    hide_settings_menu();
                    printf("KEY_MENU pressed - Hide settings menu\n");
                }
                else
                {
                    show_settings_menu();
                    printf("KEY_MENU pressed - Show settings menu\n");
                }
                update_activity_time();
            }
            last_key_menu_state = current_key_menu;
            key_menu_debounce_count = 0;
        }
    }

    // ========== KEY_OK (确认键) 处理 ==========
    if (current_key_ok == last_key_ok_state)
    {
        key_ok_debounce_count = 0;
    }
    else
    {
        key_ok_debounce_count++;
        if (key_ok_debounce_count >= debounce_threshold)
        {
            if (last_key_ok_state == 1 && current_key_ok == 0)
            {
                if (menu_visible)
                {
                    menu_confirm_selection();
                    printf("KEY_OK pressed - Menu confirm selection\n");
                }
                else
                {
                    // 非菜单模式下，OK拍照
                    turn_screen_on();
                    printf("KEY_OK pressed - Taking photo...\n");
                    int result = capture_raw_photo();
                    if (result == 0)
                    {
                        printf("Photo captured successfully\n");
                    }
                    else
                    {
                        printf("Photo capture failed\n");
                    }
                }
                update_activity_time();
            }
            last_key_ok_state = current_key_ok;
            key_ok_debounce_count = 0;
        }
    }

    // ========== KEY_X 处理 ==========
    if (current_key_x == last_key_x_state)
    {
        key_x_debounce_count = 0;
    }
    else
    {
        key_x_debounce_count++;
        if (key_x_debounce_count >= debounce_threshold)
        {
            last_key_x_state = current_key_x;
            if (current_key_x == 0)
            {
                // KEY_X 按键按下，记录按下时间
                gettimeofday(&key_x_press_start, NULL);
                key_x_long_press_triggered = false;
                printf("KEY_X pressed, starting timer...\n");
            }
            else
            {
                // KEY_X 按键释放，检查是短按还是长按
                if (!key_x_long_press_triggered)
                {
                    // 计算按键持续时间
                    long press_duration = (current_time.tv_sec - key_x_press_start.tv_sec) * 1000000 +
                                          (current_time.tv_usec - key_x_press_start.tv_usec);

                    // 验证是有效的短按（大于50ms，小于3秒）
                    if (press_duration >= 50000 && press_duration < 3000000)
                    {
                        // 短按处理：切换自动控制模式
                        if (auto_control_running)
                        {
                            printf("KEY_X短按：停止自动控制\n");
                            stop_auto_control_mode();
                        }
                        else
                        {
                            printf("KEY_X短按：启动自动控制\n");
                            start_auto_control_mode();
                        }
                        update_activity_time();
                    }
                    else if (press_duration < 50000)
                    {
                        printf("KEY_X press too short (noise), ignored\n");
                    }
                }
                printf("KEY_X released\n");
            }
            key_x_debounce_count = 0;
        }
    }

    // KEY_X 长按检测 (在按键持续按下时检查)
    if (current_key_x == 0 && last_key_x_state == 0 && !key_x_long_press_triggered)
    {
        // KEY_X 持续按下，检查是否超过5秒
        long press_duration = (current_time.tv_sec - key_x_press_start.tv_sec) * 1000000 +
                              (current_time.tv_usec - key_x_press_start.tv_usec);

        // 在RNDIS模式下禁用关机功能，避免网络干扰导致的误触
        if (press_duration >= 5000000 && get_usb_mode() != USB_MODE_RNDIS)
        {
            key_x_long_press_triggered = true;
            printf("KEY_X长按检测：执行关机...\n");
            system("poweroff");
        }
        else if (press_duration >= 5000000 && get_usb_mode() == USB_MODE_RNDIS)
        {
            key_x_long_press_triggered = true;
            printf("KEY_X长按检测：RNDIS模式下禁用关机功能\n");
        }
    }

}

// ============================================================================
// LVGL 界面初始化
// ============================================================================

/**
 * @brief 初始化 LVGL 界面
 */
void init_lvgl_ui(void)
{
    // 获取当前屏幕
    lv_obj_t *scr = lv_disp_get_scr_act(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

    // 创建图像显示区域 (使用全屏尺寸 DISPLAY_WIDTH X DISPLAY_HEIGHT)
    img_canvas = lv_img_create(scr);
    lv_obj_set_pos(img_canvas, 0, 0);
    lv_obj_set_size(img_canvas, DISPLAY_WIDTH, DISPLAY_HEIGHT); // 强制使用全屏尺寸

    // 创建系统信息标签 (左上角，显示帧率、CPU和内存占用)
    info_label = lv_label_create(scr);
    lv_label_set_text(info_label, "0.0FPS 0% 0%");
    lv_obj_set_style_text_color(info_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(info_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_bg_color(info_label, lv_color_make(0, 0, 0), 0);
    lv_obj_set_style_bg_opa(info_label, LV_OPA_50, 0);
    lv_obj_set_style_pad_all(info_label, 2, 0);
    lv_obj_align(info_label, LV_ALIGN_TOP_LEFT, 5, 5);

    // 创建时间显示标签 (右上角，显示当前时间)
    time_label = lv_label_create(scr);
    lv_label_set_text(time_label, "00:00");
    lv_obj_set_style_text_color(time_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(time_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_bg_color(time_label, lv_color_make(0, 0, 0), 0);
    lv_obj_set_style_bg_opa(time_label, LV_OPA_50, 0);
    lv_obj_set_style_pad_all(time_label, 2, 0);
    lv_obj_align(time_label, LV_ALIGN_TOP_RIGHT, -5, 5);

    // 创建子系统状态显示标签 (屏幕中央上方，用于显示 SUBSYS ON/OFF)
    subsys_status_label = lv_label_create(scr);
    lv_label_set_text(subsys_status_label, "SUBSYS OFF");
    lv_obj_set_style_text_color(subsys_status_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(subsys_status_label, &lv_font_montserrat_14, 0);  // 使用标准字体
    lv_obj_set_style_bg_color(subsys_status_label, lv_color_make(0, 0, 0), 0);
    lv_obj_set_style_bg_opa(subsys_status_label, LV_OPA_80, 0);  // 更不透明的背景
    lv_obj_set_style_border_color(subsys_status_label, lv_color_white(), 0);
    lv_obj_set_style_border_width(subsys_status_label, 2, 0);
    lv_obj_set_style_border_opa(subsys_status_label, LV_OPA_50, 0);
    lv_obj_set_style_radius(subsys_status_label, 8, 0);
    lv_obj_set_style_pad_all(subsys_status_label, 8, 0);
    lv_obj_align(subsys_status_label, LV_ALIGN_TOP_MID, 0, 40);  // 屏幕上方中央
    lv_obj_add_flag(subsys_status_label, LV_OBJ_FLAG_HIDDEN);  // 初始隐藏

    // 底部状态标签已关闭显示
    // status_label = lv_label_create(scr);
    // tcp_label = lv_label_create(scr);

    // 创建设置菜单面板 (初始隐藏) - 重新设计为固定尺寸带滚动的导航菜单
    menu_panel = lv_obj_create(scr);
    lv_obj_set_size(menu_panel, 200, 180);
    lv_obj_center(menu_panel);
    lv_obj_set_style_bg_color(menu_panel, lv_color_make(40, 40, 40), 0);
    lv_obj_set_style_bg_opa(menu_panel, LV_OPA_90, 0);
    lv_obj_set_style_border_color(menu_panel, lv_color_white(), 0);
    lv_obj_set_style_border_width(menu_panel, 2, 0);
    lv_obj_set_style_radius(menu_panel, 10, 0);
    lv_obj_set_style_pad_all(menu_panel, 12, 0);
    lv_obj_clear_flag(menu_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(menu_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(menu_panel,
                          LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_START);
    lv_obj_add_flag(menu_panel, LV_OBJ_FLAG_HIDDEN);

    // 菜单标题
    lv_obj_t *menu_title = lv_label_create(menu_panel);
    lv_label_set_text(menu_title, "Settings Menu");
    lv_obj_set_style_text_color(menu_title, lv_color_white(), 0);
    lv_obj_set_style_text_font(menu_title, &lv_font_montserrat_14, 0);
    lv_obj_set_width(menu_title, lv_pct(100));
    lv_obj_set_style_text_align(menu_title, LV_TEXT_ALIGN_CENTER, 0);

    // 创建滚动容器承载菜单条目
    menu_list_container = lv_obj_create(menu_panel);
    lv_obj_set_width(menu_list_container, lv_pct(100));
    lv_obj_set_flex_grow(menu_list_container, 1);
    lv_obj_set_style_bg_opa(menu_list_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_opa(menu_list_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(menu_list_container, 0, 0);
    lv_obj_set_style_pad_top(menu_list_container, 6, 0);
    lv_obj_set_style_pad_gap(menu_list_container, 6, 0);
    lv_obj_set_scroll_dir(menu_list_container, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(menu_list_container, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_flex_flow(menu_list_container, LV_FLEX_FLOW_COLUMN);

    // TCP 选项标签 (不是按钮，用标签显示)
    menu_tcp_btn = lv_label_create(menu_list_container);
    lv_label_set_text(menu_tcp_btn, "> TCP: OFF");
    lv_obj_set_width(menu_tcp_btn, lv_pct(100));
    lv_obj_set_style_text_color(menu_tcp_btn, lv_color_white(), 0);
    lv_obj_set_style_text_font(menu_tcp_btn, &lv_font_montserrat_14, 0);
    lv_obj_set_style_bg_color(menu_tcp_btn, lv_color_make(60, 60, 60), 0);
    lv_obj_set_style_bg_opa(menu_tcp_btn, LV_OPA_50, 0);
    lv_obj_set_style_pad_all(menu_tcp_btn, 4, 0);

    // DISPLAY 选项标签
    menu_display_btn = lv_label_create(menu_list_container);
    lv_label_set_text(menu_display_btn, "  DISPLAY: ON");
    lv_obj_set_width(menu_display_btn, lv_pct(100));
    lv_obj_set_style_text_color(menu_display_btn, lv_color_white(), 0);
    lv_obj_set_style_text_font(menu_display_btn, &lv_font_montserrat_14, 0);
    lv_obj_set_style_bg_color(menu_display_btn, lv_color_make(20, 20, 20), 0);
    lv_obj_set_style_bg_opa(menu_display_btn, LV_OPA_30, 0);
    lv_obj_set_style_pad_all(menu_display_btn, 4, 0);

    // EXPOSURE 选项标签
    menu_exposure_btn = lv_label_create(menu_list_container);
    lv_label_set_text(menu_exposure_btn, "  EXPOSURE: 128");
    lv_obj_set_width(menu_exposure_btn, lv_pct(100));
    lv_obj_set_style_text_color(menu_exposure_btn, lv_color_white(), 0);
    lv_obj_set_style_text_font(menu_exposure_btn, &lv_font_montserrat_14, 0);
    lv_obj_set_style_bg_color(menu_exposure_btn, lv_color_make(20, 20, 20), 0);
    lv_obj_set_style_bg_opa(menu_exposure_btn, LV_OPA_30, 0);
    lv_obj_set_style_pad_all(menu_exposure_btn, 4, 0);

    // GAIN 选项标签
    menu_gain_btn = lv_label_create(menu_list_container);
    lv_label_set_text(menu_gain_btn, "  GAIN: 128");
    lv_obj_set_width(menu_gain_btn, lv_pct(100));
    lv_obj_set_style_text_color(menu_gain_btn, lv_color_white(), 0);
    lv_obj_set_style_text_font(menu_gain_btn, &lv_font_montserrat_14, 0);
    lv_obj_set_style_bg_color(menu_gain_btn, lv_color_make(20, 20, 20), 0);
    lv_obj_set_style_bg_opa(menu_gain_btn, LV_OPA_30, 0);
    lv_obj_set_style_pad_all(menu_gain_btn, 4, 0);

    // USB CONFIG 选项标签
    menu_usb_config_btn = lv_label_create(menu_list_container);
    lv_label_set_text(menu_usb_config_btn, "  USB: ADB");
    lv_obj_set_width(menu_usb_config_btn, lv_pct(100));
    lv_obj_set_style_text_color(menu_usb_config_btn, lv_color_white(), 0);
    lv_obj_set_style_text_font(menu_usb_config_btn, &lv_font_montserrat_14, 0);
    lv_obj_set_style_bg_color(menu_usb_config_btn, lv_color_make(20, 20, 20), 0);
    lv_obj_set_style_bg_opa(menu_usb_config_btn, LV_OPA_30, 0);
    lv_obj_set_style_pad_all(menu_usb_config_btn, 4, 0);

    // HEATER1 选项标签
    menu_heater1_btn = lv_label_create(menu_list_container);
    lv_label_set_text(menu_heater1_btn, "  HEATER1: AUTO");
    lv_obj_set_width(menu_heater1_btn, lv_pct(100));
    lv_obj_set_style_text_color(menu_heater1_btn, lv_color_white(), 0);
    lv_obj_set_style_text_font(menu_heater1_btn, &lv_font_montserrat_14, 0);
    lv_obj_set_style_bg_color(menu_heater1_btn, lv_color_make(20, 20, 20), 0);
    lv_obj_set_style_bg_opa(menu_heater1_btn, LV_OPA_30, 0);
    lv_obj_set_style_pad_all(menu_heater1_btn, 4, 0);

    // HEATER2 选项标签
    menu_heater2_btn = lv_label_create(menu_list_container);
    lv_label_set_text(menu_heater2_btn, "  HEATER2: AUTO");
    lv_obj_set_width(menu_heater2_btn, lv_pct(100));
    lv_obj_set_style_text_color(menu_heater2_btn, lv_color_white(), 0);
    lv_obj_set_style_text_font(menu_heater2_btn, &lv_font_montserrat_14, 0);
    lv_obj_set_style_bg_color(menu_heater2_btn, lv_color_make(20, 20, 20), 0);
    lv_obj_set_style_bg_opa(menu_heater2_btn, LV_OPA_30, 0);
    lv_obj_set_style_pad_all(menu_heater2_btn, 4, 0);

    // PUMP 选项标签
    menu_pump_btn = lv_label_create(menu_list_container);
    lv_label_set_text(menu_pump_btn, "  PUMP: AUTO");
    lv_obj_set_width(menu_pump_btn, lv_pct(100));
    lv_obj_set_style_text_color(menu_pump_btn, lv_color_white(), 0);
    lv_obj_set_style_text_font(menu_pump_btn, &lv_font_montserrat_14, 0);
    lv_obj_set_style_bg_color(menu_pump_btn, lv_color_make(20, 20, 20), 0);
    lv_obj_set_style_bg_opa(menu_pump_btn, LV_OPA_30, 0);
    lv_obj_set_style_pad_all(menu_pump_btn, 4, 0);

    // LASER 选项标签
    menu_laser_btn = lv_label_create(menu_list_container);
    lv_label_set_text(menu_laser_btn, "  LASER: AUTO");
    lv_obj_set_width(menu_laser_btn, lv_pct(100));
    lv_obj_set_style_text_color(menu_laser_btn, lv_color_white(), 0);
    lv_obj_set_style_text_font(menu_laser_btn, &lv_font_montserrat_14, 0);
    lv_obj_set_style_bg_color(menu_laser_btn, lv_color_make(20, 20, 20), 0);
    lv_obj_set_style_bg_opa(menu_laser_btn, LV_OPA_30, 0);
    lv_obj_set_style_pad_all(menu_laser_btn, 4, 0);

    // 创建子系统状态面板 (屏幕底部)
    subsys_panel = lv_obj_create(scr);
    lv_obj_set_size(subsys_panel, DISPLAY_WIDTH, 30); // 减小高度，只需要一行文字
    lv_obj_align(subsys_panel, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(subsys_panel, lv_color_make(0, 0, 0), 0);
    lv_obj_set_style_bg_opa(subsys_panel, LV_OPA_70, 0);
    lv_obj_set_style_border_width(subsys_panel, 0, 0);
    lv_obj_set_style_pad_all(subsys_panel, 5, 0);

    // 禁用滚动条
    lv_obj_clear_flag(subsys_panel, LV_OBJ_FLAG_SCROLLABLE);

    // 创建激光器状态标签
    laser_status_label = lv_label_create(subsys_panel);
    lv_label_set_text(laser_status_label, "L");
    lv_obj_set_style_text_color(laser_status_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(laser_status_label, &lv_font_montserrat_12, 0);
    lv_obj_align(laser_status_label, LV_ALIGN_LEFT_MID, 10, 0);

    // 创建第一个分隔符
    separator1_label = lv_label_create(subsys_panel);
    lv_label_set_text(separator1_label, "/");
    lv_obj_set_style_text_color(separator1_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(separator1_label, &lv_font_montserrat_12, 0);
    lv_obj_align_to(separator1_label, laser_status_label, LV_ALIGN_OUT_RIGHT_MID, 5, 0);

    // 创建气泵状态标签
    pump_status_label = lv_label_create(subsys_panel);
    lv_label_set_text(pump_status_label, "P");
    lv_obj_set_style_text_color(pump_status_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(pump_status_label, &lv_font_montserrat_12, 0);
    lv_obj_align_to(pump_status_label, separator1_label, LV_ALIGN_OUT_RIGHT_MID, 5, 0);

    // 创建第二个分隔符
    separator2_label = lv_label_create(subsys_panel);
    lv_label_set_text(separator2_label, "/");
    lv_obj_set_style_text_color(separator2_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(separator2_label, &lv_font_montserrat_12, 0);
    lv_obj_align_to(separator2_label, pump_status_label, LV_ALIGN_OUT_RIGHT_MID, 5, 0);

    // 创建加热器1状态标签
    heater1_status_label = lv_label_create(subsys_panel);
    lv_label_set_text(heater1_status_label, "H1:--°C");
    lv_obj_set_style_text_color(heater1_status_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(heater1_status_label, &lv_font_montserrat_12, 0);
    lv_obj_align_to(heater1_status_label, separator2_label, LV_ALIGN_OUT_RIGHT_MID, 5, 0);

    // 创建第三个分隔符
    separator3_label = lv_label_create(subsys_panel);
    lv_label_set_text(separator3_label, "/");
    lv_obj_set_style_text_color(separator3_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(separator3_label, &lv_font_montserrat_12, 0);
    lv_obj_align_to(separator3_label, heater1_status_label, LV_ALIGN_OUT_RIGHT_MID, 25, 0);

    // 创建加热器2状态标签
    heater2_status_label = lv_label_create(subsys_panel);
    lv_label_set_text(heater2_status_label, "H2:--°C");
    lv_obj_set_style_text_color(heater2_status_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(heater2_status_label, &lv_font_montserrat_12, 0);
    lv_obj_align_to(heater2_status_label, separator3_label, LV_ALIGN_OUT_RIGHT_MID, 10, 0);

    // 初始化时间更新时间戳
    gettimeofday(&last_time_update, NULL);

    printf("LVGL UI initialized (landscape mode: %dx%d)\n", DISPLAY_WIDTH, DISPLAY_HEIGHT);
}

// ============================================================================
// 主函数
// ============================================================================

int main(int argc, char *argv[])
{
    printf("LVGL Camera Display System Starting...\n");

    // 解析命令行参数
    int parse_result = parse_arguments(argc, argv);
    if (parse_result == 1)
    {
        // 显示帮助后正常退出
        return 0;
    }
    else if (parse_result == -1)
    {
        // 参数解析错误
        return -1;
    }

    // 设置信号处理
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // 初始化默认配置
    init_default_config(&current_config);

    // 尝试加载配置文件
    printf("Loading configuration from %s...\n", CONFIG_FILE_PATH);
    if (load_config_file(&current_config) == 0)
    {
        printf("Configuration loaded successfully\n");
        config_loaded = 1;
        // 应用配置到全局变量
        apply_config(&current_config);
    }
    else
    {
        printf("Using default configuration\n");
        config_loaded = 0;
    }

    // 初始化 LVGL
    lv_init();

    // 初始化LVGL文件系统 (用于加载图标)
    lv_fs_stdio_init();

    // 检查显示配置
    check_display_config();

    // 初始化帧缓冲设备
    fbdev_init();

    // 初始化LCD设备 (用于电源管理)
    printf("Initializing LCD device for power management...\n");
    if (fbtft_lcd_init(&lcd_device, "/dev/fb0") == 0)
    {
        lcd_initialized = 1;
        printf("LCD device initialized successfully\n");
    }
    else
    {
        printf("Warning: LCD device initialization failed, power management disabled\n");
        lcd_initialized = 0;
    }

    // 创建 LVGL 显示缓冲区
    static lv_color_t buf[DISP_BUF_SIZE];
    static lv_disp_draw_buf_t disp_buf;
    lv_disp_draw_buf_init(&disp_buf, buf, NULL, DISP_BUF_SIZE);

    // 注册显示驱动 (强制横屏模式: DISPLAY_WIDTH X DISPLAY_HEIGHT)
    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.draw_buf = &disp_buf;
    disp_drv.flush_cb = fbdev_flush;
    disp_drv.hor_res = DISPLAY_WIDTH;  // 强制设置横屏宽度
    disp_drv.ver_res = DISPLAY_HEIGHT; // 强制设置横屏高度

    // 尝试设置旋转（如果支持）
    // disp_drv.rotated = LV_DISP_ROT_90;  // 如果需要旋转90度

    lv_disp_drv_register(&disp_drv);

    // 初始化 GPIO
    if (DEV_ModuleInit() != 0)
    {
        printf("Failed to initialize GPIO\n");
        return -1;
    }

#if (BATTERY_SHOW)
    // 初始化 INA219 电池监测 (可选，失败不影响主要功能)
    if (init_ina219() == 0)
    {
        printf("INA219 battery monitoring initialized\n");
    }
    else
    {
        printf("Warning: INA219 initialization failed, battery monitoring disabled\n");
    }
#endif

    // 初始化子系统通信
    init_subsystem(); // 总是成功，但可能以离线模式运行

    if (subsys_handle)
    {
        printf("子系统通信初始化成功\n");
    }
    else
    {
        printf("警告: 子系统通信不可用，将以离线模式运行\n");
    }

    // 初始化 libMedia
    if (libmedia_init() != 0)
    {
        printf("Failed to initialize libMedia\n");
        goto cleanup;
    }

    // 在创建会话之前，检查设备是否可用
    printf("Checking camera device availability...\n");
    
    // 尝试简单打开和关闭设备来检查可用性
    int test_fd = open(DEFAULT_CAMERA_DEVICE, O_RDWR);
    if (test_fd < 0)
    {
        printf("Error: Cannot open camera device %s: %s\n", DEFAULT_CAMERA_DEVICE, strerror(errno));
        printf("Please check if:\n");
        printf("1. The camera device exists\n");
        printf("2. No other process is using the camera\n");
        printf("3. You have proper permissions\n");
        goto cleanup;
    }
    close(test_fd);
    printf("Camera device %s is accessible\n", DEFAULT_CAMERA_DEVICE);

    // 配置摄像头会话 (使用命令行参数或默认值)
    media_session_config_t config = {
        .device_path = DEFAULT_CAMERA_DEVICE,
        .format = {
            .width = camera_width,
            .height = camera_height,
            .pixelformat = CAMERA_PIXELFORMAT,
            .num_planes = 1,
            .plane_size = {camera_width * camera_height * 5 / 4} // RAW10: 10位/像素 = 1.25字节/像素
        },
        .buffer_count = BUFFER_COUNT,
        .use_multiplanar = 1, // 多平面模式
        .nonblocking = 0};

    // 创建媒体会话
    media_session = libmedia_create_session(&config);
    if (!media_session)
    {
        printf("Failed to create media session: %s\n",
               libmedia_get_error_string(libmedia_get_last_error()));
        goto cleanup;
    }

    // 启动摄像头流
    if (libmedia_start_session(media_session) < 0)
    {
        printf("Failed to start media session: %s\n",
               libmedia_get_error_string(libmedia_get_last_error()));
        goto cleanup;
    }

    printf("Camera session started successfully\n");

    // ========================================================================
    // 裁剪功能实现说明
    // ========================================================================
    // 由于Rockchip CSI驱动的特殊性，标准的V4L2 S_SELECTION不能直接使用
    // 错误: "csi size err, intstat:0x1000200, size:0xc80500"
    // 
    // 替代方案：
    // 1. 使用软件裁剪：在图像处理阶段进行裁剪
    // 2. 调研Rockchip专用的裁剪API
    // 3. 或者使用RGA（Rockchip Graphics Accelerator）进行硬件裁剪
    //
    // 当前状态：配置文件中的crop_top和crop_left会被加载但不应用
    if (crop_top > 0 || crop_left > 0) {
        printf("Note: Crop configuration loaded (top=%d, left=%d) but not applied\n", crop_top, crop_left);
        printf("      Hardware crop is disabled due to Rockchip CSI driver limitations\n");
        printf("      Consider implementing software crop or RGA-based crop in the future\n");
    }

    // 初始化 LVGL 界面
    init_lvgl_ui();

    // 立即更新时间和电池显示
    update_time_display();

    // 初始化曝光和增益控制
    init_camera_controls();

    // 初始化USB配置模块
    printf("Initializing USB configuration module...\n");
    if (init_usb_config() == 0)
    {
        printf("USB configuration module initialized\n");
    }
    else
    {
        printf("Warning: USB configuration module initialization failed\n");
    }

    // 如果配置文件已加载，应用曝光和增益值到硬件
    if (config_loaded)
    {
        printf("Applying loaded configuration to camera hardware...\n");
        update_exposure_value(current_config.exposure);
        update_gain_value(current_config.gain);
        printf("Configuration applied to camera hardware\n");
    }

    // 初始化帧率统计
    gettimeofday(&last_fps_time, NULL);

    // 初始化屏幕活动时间
    update_activity_time();

    // 启动摄像头采集线程 (最高优先级)
    pthread_t camera_tid;
    pthread_attr_t camera_attr;
    struct sched_param camera_param;
    
    // 初始化线程属性
    pthread_attr_init(&camera_attr);
    pthread_attr_setdetachstate(&camera_attr, PTHREAD_CREATE_JOINABLE);
    
    // 设置调度策略为FIFO，优先级最高
    pthread_attr_setschedpolicy(&camera_attr, SCHED_FIFO);
    camera_param.sched_priority = sched_get_priority_max(SCHED_FIFO);
    pthread_attr_setschedparam(&camera_attr, &camera_param);
    pthread_attr_setinheritsched(&camera_attr, PTHREAD_EXPLICIT_SCHED);
    
    printf("Setting camera thread priority to: %d (SCHED_FIFO)\n", camera_param.sched_priority);
    
    if (pthread_create(&camera_tid, &camera_attr, camera_thread, NULL) != 0)
    {
        printf("Failed to create camera thread with high priority, trying normal priority...\n");
        // 如果高优先级失败，尝试默认优先级
        pthread_attr_destroy(&camera_attr);
        pthread_attr_init(&camera_attr);
        if (pthread_create(&camera_tid, &camera_attr, camera_thread, NULL) != 0)
        {
            printf("Failed to create camera thread\n");
            pthread_attr_destroy(&camera_attr);
            goto cleanup;
        }
    }
    
    pthread_attr_destroy(&camera_attr);

    // 如果命令行启用了TCP，启动TCP服务器线程
    if (tcp_enabled)
    {
        printf("Starting TCP server thread as enabled via command line...\n");
        
        // 先创建TCP服务器
        server_fd = create_server(DEFAULT_PORT);
        if (server_fd >= 0)
        {
            pthread_attr_t tcp_attr;
            struct sched_param tcp_param;
            
            // 初始化TCP线程属性
            pthread_attr_init(&tcp_attr);
            pthread_attr_setdetachstate(&tcp_attr, PTHREAD_CREATE_JOINABLE);
            
            // 设置调度策略为FIFO，中等优先级
            pthread_attr_setschedpolicy(&tcp_attr, SCHED_FIFO);
            tcp_param.sched_priority = sched_get_priority_max(SCHED_FIFO) / 2;
            pthread_attr_setschedparam(&tcp_attr, &tcp_param);
            pthread_attr_setinheritsched(&tcp_attr, PTHREAD_EXPLICIT_SCHED);
            
            printf("Setting TCP thread priority to: %d (SCHED_FIFO)\n", tcp_param.sched_priority);
            
            if (pthread_create(&tcp_thread_id, &tcp_attr, tcp_sender_thread, NULL) == 0)
            {
                printf("TCP server started successfully with priority %d\n", tcp_param.sched_priority);
            }
            else
            {
                printf("Failed to create TCP thread with priority, trying normal priority...\n");
                // 如果优先级设置失败，尝试默认优先级
                pthread_attr_destroy(&tcp_attr);
                pthread_attr_init(&tcp_attr);
                if (pthread_create(&tcp_thread_id, &tcp_attr, tcp_sender_thread, NULL) == 0)
                {
                    printf("TCP server started successfully with normal priority\n");
                }
                else
                {
                    printf("Failed to create TCP thread\n");
                    close(server_fd);
                    server_fd = -1;
                    tcp_enabled = 0;
                }
            }
            
            pthread_attr_destroy(&tcp_attr);
        }
        else
        {
            printf("Failed to create TCP server socket\n");
            tcp_enabled = 0;
        }
    }

    printf("System initialized successfully\n");
    
    // 设置主线程为低优先级，让摄像头线程优先执行
    struct sched_param main_param;
    main_param.sched_priority = 0;  // 最低优先级
    if (pthread_setschedparam(pthread_self(), SCHED_OTHER, &main_param) == 0)
    {
        printf("Main thread priority set to: %d (SCHED_OTHER)\n", main_param.sched_priority);
    }
    else
    {
        printf("Warning: Failed to set main thread priority\n");
    }
    
    printf("Display: %dx%d (forced landscape mode)\n", DISPLAY_WIDTH, DISPLAY_HEIGHT);
    printf("Camera: %dx%d (RAW10) on %s\n", camera_width, camera_height, DEFAULT_CAMERA_DEVICE);
    printf("Scaling: Width-aligned to %d px, maintaining aspect ratio\n", DISPLAY_WIDTH);
    printf("Performance optimizations enabled:\n");
    printf("  - Display update rate limited to 30 FPS\n");
    printf("  - Non-blocking frame mutex for better key response\n");
    printf("  - Optimized key debouncing (3 samples)\n");
    printf("  - Dynamic buffer allocation for different resolutions\n");
    printf("  - Reduced debug output for better performance\n");
    printf("Controls:\n");
    printf("  当菜单隐藏时:\n");
    printf("  KEY_MENU (PIN %d)  - 显示设置菜单\n", KEY_MENU_PIN);
    printf("  KEY_OK (PIN %d)    - 拍照\n", KEY_OK_PIN);
    printf("  KEY_X (PIN %d)     - 短按: 切换自动控制 / 长按: 关机\n", KEY_X_PIN);
    printf("\n  当菜单显示时:\n");
    printf("  KEY_UP/RIGHT       - 菜单导航上/增加数值(调整模式)\n");
    printf("  KEY_DOWN/LEFT      - 菜单导航下/减少数值(调整模式)\n");
    printf("  KEY_OK             - 确认选择\n");
    printf("  KEY_MENU           - 隐藏菜单\n");
    printf("  Ctrl+C - Exit\n");
    printf("Screen Management:\n");
    printf("  - Auto-sleep after 5s when display is OFF\n");
    printf("  - Wake with any key press\n");
    printf("Function Independence:\n");
    printf("  - Camera: Always running (captures frames continuously)\n");
    printf("  - Display: Controlled by KEY_LEFT (ON/OFF) or Settings Menu\n");
    printf("  - TCP: Controlled by KEY_RIGHT (independent of display status) or Settings Menu\n");
    printf("  - Settings Menu: Controlled by KEY_MENU (virtual menu with TCP & DISPLAY options)\n");
    printf("  - Time Display: Real-time clock in top-right corner (updates every minute)\n");
    printf("TCP Server: %s:%d (%s)\n", DEFAULT_SERVER_IP, DEFAULT_PORT, 
           tcp_enabled ? "enabled" : "disabled by default");

    // 主循环
    struct timeval last_display_update = {0};
    struct timeval last_status_update = {0};
    struct timeval last_info_update = {0};
    gettimeofday(&last_display_update, NULL);
    gettimeofday(&last_status_update, NULL);
    gettimeofday(&last_info_update, NULL);

    while (!exit_flag)
    {
        struct timeval current_time;
        gettimeofday(&current_time, NULL);

        // 处理 LVGL 任务 (高优先级，每次循环都执行)
        lv_timer_handler();

        // 再次检查退出标志
        if (exit_flag)
            break;

        // 处理按键 (高优先级，每次循环都执行)
        handle_keys();

        // 再次检查退出标志
        if (exit_flag)
            break;

        // 限制图像显示更新频率到30FPS - 进一步优化
        long display_time_diff = (current_time.tv_sec - last_display_update.tv_sec) * 1000000 +
                                 (current_time.tv_usec - last_display_update.tv_usec);

        if (display_time_diff >= 25000)
        { // 优化为25ms以提高到40FPS潜力，实际受限于摄像头
            // 只在屏幕开启且显示启用时更新图像显示
            // 当有TCP客户端连接时，暂停屏幕显示以减少系统负载
            if (screen_on && display_enabled && !client_connected)
            {
                update_image_display();
            }
            last_display_update = current_time;
        }

        // 更新子系统状态显示 - 降低频率到每200ms
        // 当有TCP客户端连接时，暂停子系统查询以减少系统负载
        long status_time_diff = (current_time.tv_sec - last_status_update.tv_sec) * 1000000 +
                                (current_time.tv_usec - last_status_update.tv_usec);
        if (status_time_diff >= 200000 && !client_connected)
        { // 200ms，减少干扰，且TCP连接时完全暂停
            // 更新设备信息（包含温度过滤）
            if (subsys_handle) {
                get_device_info_with_filter(subsys_handle, &device_info);
            }

            enforce_manual_device_modes();
            
            update_subsys_status_display();
            
            // 检查屏幕自动关闭（摄像头暂停5秒后）
            check_screen_timeout();
            
            last_status_update = current_time;
        }

        // 更新系统信息和时间显示 - 降低频率到每秒
        // 当有TCP客户端连接时，暂停系统信息更新以减少系统负载
        long info_time_diff = (current_time.tv_sec - last_info_update.tv_sec) * 1000000 +
                              (current_time.tv_usec - last_info_update.tv_usec);
        if (info_time_diff >= 1000000 && !client_connected)
        { // 1秒，且TCP连接时完全暂停
            update_system_info();
            if (screen_on) {
                update_time_display();
            }
            
            // 检查是否需要隐藏子系统状态
            hide_subsys_status_delayed();
            
            last_info_update = current_time;
        }

        // 优化休眠时间：TCP连接时减少休眠以提高传输效率
        if (client_connected) {
            usleep(100); // TCP连接时仅100微秒休眠，最大化传输效率
        } else {
            usleep(1000); // 正常情况下1ms休眠
        }
    }

    printf("Main loop exited, shutting down...\n");

    // 停止TCP传输
    tcp_enabled = 0;
    if (client_connected && client_fd >= 0)
    {
        close(client_fd);
        client_connected = 0;
    }
    if (server_fd >= 0)
    {
        close(server_fd);
        server_fd = -1;
    }

    // 清理相机控制
    cleanup_camera_controls();

    // 确保停止媒体会话，避免阻塞
    printf("Stopping media session...\n");
    if (media_session)
    {
        libmedia_stop_session(media_session);
    }

    // 等待摄像头线程结束 (设置更短的超时)
    printf("Waiting for camera thread to exit...\n");
    struct timespec timeout;
    clock_gettime(CLOCK_REALTIME, &timeout);
    timeout.tv_sec += 1; // 缩短超时时间到1秒

    int join_result = pthread_timedjoin_np(camera_tid, NULL, &timeout);
    if (join_result == ETIMEDOUT)
    {
        printf("Warning: Camera thread did not exit within timeout, canceling...\n");
        pthread_cancel(camera_tid);
        
        // 给一个很短的时间让取消操作完成
        timeout.tv_sec = time(NULL) + 1;
        timeout.tv_nsec = 0;
        
        int cancel_join_result = pthread_timedjoin_np(camera_tid, NULL, &timeout);
        if (cancel_join_result == ETIMEDOUT)
        {
            printf("Warning: Camera thread cancel timeout, forcing exit\n");
            // 不再等待，强制继续清理
        }
        else if (cancel_join_result == 0)
        {
            printf("Camera thread canceled successfully\n");
        }
    }
    else if (join_result != 0)
    {
        printf("Warning: pthread_join failed: %d\n", join_result);
    }
    else
    {
        printf("Camera thread joined successfully\n");
    }

cleanup:
    // 清理子系统资源
    printf("Cleaning up subsystem...\n");
    cleanup_subsystem();

    // 等待TCP线程结束
    if (tcp_enabled)
    {
        printf("Waiting for TCP thread to exit...\n");
        tcp_enabled = 0;
        pthread_cond_broadcast(&frame_ready);
        void *tcp_ret;
        if (pthread_join(tcp_thread_id, &tcp_ret) == 0)
        {
            printf("TCP thread exited successfully\n");
        }
    }

    // 清理TCP资源
    if (client_connected && client_fd >= 0)
    {
        close(client_fd);
        client_connected = 0;
    }
    if (server_fd >= 0)
    {
        close(server_fd);
        server_fd = -1;
    }

    // 清理动态分配的图像缓冲区
    printf("Cleaning up image buffers...\n");
    cleanup_image_buffers();

    // 清理当前帧数据
    printf("Cleaning up frame data...\n");
    pthread_mutex_lock(&frame_mutex);
    if (current_frame.data && media_session)  // 确保 media_session 不为 NULL
    {
        libmedia_session_release_frame(media_session, &current_frame);
        current_frame.data = NULL;
    }
    else if (current_frame.data)
    {
        // 如果 media_session 为 NULL 但有帧数据，只清空指针
        printf("Warning: Clearing frame data without media session\n");
        current_frame.data = NULL;
    }
    pthread_mutex_unlock(&frame_mutex);

    // 清理媒体会话
    printf("Cleaning up media session...\n");
    if (media_session)
    {
        libmedia_stop_session(media_session);
        libmedia_destroy_session(media_session);
        media_session = NULL;
    }

    // 清理 libMedia
    printf("Deinitializing libMedia...\n");
    libmedia_deinit();

    // 清理LCD设备
    if (lcd_initialized)
    {
        printf("Deinitializing LCD device...\n");
        fbtft_lcd_deinit(&lcd_device);
        lcd_initialized = 0;
    }

#if (BATTERY_SHOW)
    // 清理 INA219 电池监测
    cleanup_ina219();
#endif

    // 清理USB配置模块
    printf("Cleaning up USB configuration...\n");
    cleanup_usb_config();

    // 清理 GPIO
    printf("Cleaning up GPIO...\n");
    DEV_ModuleExit();

    // 清理互斥锁和条件变量
    printf("Cleaning up synchronization objects...\n");
    pthread_mutex_destroy(&frame_mutex);
    pthread_cond_destroy(&frame_ready);

    printf("System shutdown complete\n");
    fflush(stdout);
    return 0;
}

/**
 * @brief 更新时间和电池显示 (时间每分钟更新，电池每5秒更新)
 */
void update_time_display(void)
{
    if (!time_label || !screen_on)
        return;

    struct timeval current_time;
    gettimeofday(&current_time, NULL);

    // 检查时间显示是否需要更新（每分钟）
    long time_diff = (current_time.tv_sec - last_time_update.tv_sec) * 1000000 +
                     (current_time.tv_usec - last_time_update.tv_usec);

    // 获取当前时间字符串
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char time_str[32];
    strftime(time_str, 16, "%H:%M", tm_info);
    char display_str[64];

#if (BATTERY_SHOW)
    static struct timeval last_battery_update = {0, 0};
    long battery_time_diff = (current_time.tv_sec - last_battery_update.tv_sec) * 1000000 +
                             (current_time.tv_usec - last_battery_update.tv_usec);

    // 初始化或更新电池状态（每5秒更新一次）
    static float last_battery_percentage = -1.0f;
    static bool need_display_update = false;
    bool is_charging = false;

    if (battery_time_diff >= 5000000 || last_battery_percentage < 0)
    { // 5秒
        if (is_ina219_initialized())
        {
            update_battery_status();
            float new_percentage = get_battery_percentage();

            // 只有当电池百分比变化时才更新显示
            if (fabsf(new_percentage - last_battery_percentage) >= 0.1f || last_battery_percentage < 0)
            {
                last_battery_percentage = new_percentage;
                need_display_update = true;

                if (get_battery_current() < 0.0f)
                {
                    is_charging = true; // 负电流表示充电
                }
                else
                {
                    is_charging = false; // 正电流表示放电
                }

                printf("Battery status updated: %.1f%%\n", (double)last_battery_percentage);
            }
            last_battery_update = current_time;
        }
        else
        {
            printf("Cannot access battery percentage - INA219 not initialized\n");
            if (last_battery_percentage != -1.0f)
            {
                last_battery_percentage = -1.0f;
                need_display_update = true;
            }
        }
    }

    // 更新显示内容（增加缓冲区大小以避免截断警告）
    if (last_battery_percentage >= 0)
    {
        // 根据电池电量决定显示颜色
        if (last_battery_percentage < 20.0f && !is_charging)
        {
            // 电量低于20%时显示红色
            snprintf(display_str, sizeof(display_str), "%s  #ff0000 %.0f%%#", time_str, (double)last_battery_percentage);
        }
        else if (is_charging)
        {
            // 充电状态显示绿色
            snprintf(display_str, sizeof(display_str), "%s  #00ff00 %.0f%%#", time_str, (double)last_battery_percentage);
        }
        else
        {
            // 正常电量显示白色
            snprintf(display_str, sizeof(display_str), "%s  #ffffff %.0f%%#", time_str, (double)last_battery_percentage);
        }
    }
    else
    {
        snprintf(display_str, sizeof(display_str), "%s  #ffffff N/A%%#", time_str);
    }

    // 更新显示：时间每分钟更新 OR 电池状态有变化时更新
    if (time_diff >= 60000000 || need_display_update)
    {
        // 启用富文本模式以支持颜色
        lv_label_set_recolor(time_label, true);
        lv_label_set_text(time_label, display_str);

        if (time_diff >= 60000000)
        {
            last_time_update = current_time;
            printf("Time display updated: %s\n", time_str);
        }

        if (need_display_update)
        {
            need_display_update = false;
            printf("Display updated due to battery status change\n");
        }
    }
#else
    snprintf(display_str, sizeof(display_str), "%s  #ffffff #", time_str);

    if (time_diff >= 60000000)
    {
        // 启用富文本模式以支持颜色
        lv_label_set_recolor(time_label, true);
        lv_label_set_text(time_label, display_str);
        last_time_update = current_time;
    }
#endif
}

/**
 * @brief 显示设置菜单
 */
void show_settings_menu(void)
{
    if (!menu_panel)
        return;

    menu_visible = 1;
    menu_selected_item = 0; // 默认选择第一项 (TCP)
    lv_obj_clear_flag(menu_panel, LV_OBJ_FLAG_HIDDEN);

    if (menu_list_container)
    {
        lv_obj_scroll_to_y(menu_list_container, 0, LV_ANIM_OFF);
    }

    // 更新菜单内容并刷新选择状态
    update_menu_selection();

    printf("Settings menu opened, selected item: %d\n", menu_selected_item);
}

/**
 * @brief 隐藏设置菜单
 */
void hide_settings_menu(void)
{
    if (!menu_panel)
        return;

    menu_visible = 0;
    lv_obj_add_flag(menu_panel, LV_OBJ_FLAG_HIDDEN);

    printf("Settings menu closed\n");
}

/**
 * @brief 更新菜单选择状态的视觉显示
 */
static lv_obj_t *menu_item_object(int item)
{
    switch (item)
    {
    case MENU_ITEM_TCP:
        return menu_tcp_btn;
    case MENU_ITEM_DISPLAY:
        return menu_display_btn;
    case MENU_ITEM_EXPOSURE:
        return menu_exposure_btn;
    case MENU_ITEM_GAIN:
        return menu_gain_btn;
    case MENU_ITEM_USB:
        return menu_usb_config_btn;
    case MENU_ITEM_HEATER1:
        return menu_heater1_btn;
    case MENU_ITEM_HEATER2:
        return menu_heater2_btn;
    case MENU_ITEM_PUMP:
        return menu_pump_btn;
    case MENU_ITEM_LASER:
        return menu_laser_btn;
    default:
        return NULL;
    }
}

void update_menu_selection(void)
{
    if (!menu_visible || !menu_list_container || !menu_tcp_btn || !menu_display_btn || !menu_exposure_btn || !menu_gain_btn || !menu_usb_config_btn ||
        !menu_heater1_btn || !menu_heater2_btn || !menu_pump_btn || !menu_laser_btn)
        return;

    // 重置所有选项的背景
    lv_obj_set_style_bg_color(menu_tcp_btn, lv_color_make(20, 20, 20), 0);
    lv_obj_set_style_bg_opa(menu_tcp_btn, LV_OPA_30, 0);
    lv_obj_set_style_bg_color(menu_display_btn, lv_color_make(20, 20, 20), 0);
    lv_obj_set_style_bg_opa(menu_display_btn, LV_OPA_30, 0);
    lv_obj_set_style_bg_color(menu_exposure_btn, lv_color_make(20, 20, 20), 0);
    lv_obj_set_style_bg_opa(menu_exposure_btn, LV_OPA_30, 0);
    lv_obj_set_style_bg_color(menu_gain_btn, lv_color_make(20, 20, 20), 0);
    lv_obj_set_style_bg_opa(menu_gain_btn, LV_OPA_30, 0);
    lv_obj_set_style_bg_color(menu_usb_config_btn, lv_color_make(20, 20, 20), 0);
    lv_obj_set_style_bg_opa(menu_usb_config_btn, LV_OPA_30, 0);
    lv_obj_set_style_bg_color(menu_heater1_btn, lv_color_make(20, 20, 20), 0);
    lv_obj_set_style_bg_opa(menu_heater1_btn, LV_OPA_30, 0);
    lv_obj_set_style_bg_color(menu_heater2_btn, lv_color_make(20, 20, 20), 0);
    lv_obj_set_style_bg_opa(menu_heater2_btn, LV_OPA_30, 0);
    lv_obj_set_style_bg_color(menu_pump_btn, lv_color_make(20, 20, 20), 0);
    lv_obj_set_style_bg_opa(menu_pump_btn, LV_OPA_30, 0);
    lv_obj_set_style_bg_color(menu_laser_btn, lv_color_make(20, 20, 20), 0);
    lv_obj_set_style_bg_opa(menu_laser_btn, LV_OPA_30, 0);

    // 更新文本内容
    char tcp_text[32];
    char display_text[32];
    char exposure_text[32];
    char gain_text[32];
    char usb_text[32];
    char heater1_text[40];
    char heater2_text[40];
    char pump_text[32];
    char laser_text[32];

    // TCP状态根据USB模式动态显示
    if (is_tcp_available())
    {
        snprintf(tcp_text, sizeof(tcp_text), "  TCP: %s", tcp_enabled ? "ON" : "OFF");
    }
    else
    {
        snprintf(tcp_text, sizeof(tcp_text), "  TCP: N/A");
    }

    snprintf(display_text, sizeof(display_text), "  DISPLAY: %s", display_enabled ? "ON" : "OFF");
    snprintf(exposure_text, sizeof(exposure_text), "  EXPOSURE: %d", current_exposure);
    snprintf(gain_text, sizeof(gain_text), "  GAIN: %d", current_gain);
    snprintf(usb_text, sizeof(usb_text), "  USB: %s", get_usb_mode_name(get_usb_mode()));
    snprintf(heater1_text, sizeof(heater1_text), "  HEATER1: %s", device_mode_to_string(device_modes[DEVICE_CTRL_HEATER1]));
    snprintf(heater2_text, sizeof(heater2_text), "  HEATER2: %s", device_mode_to_string(device_modes[DEVICE_CTRL_HEATER2]));
    snprintf(pump_text, sizeof(pump_text), "  PUMP: %s", device_mode_to_string(device_modes[DEVICE_CTRL_PUMP]));
    snprintf(laser_text, sizeof(laser_text), "  LASER: %s", device_mode_to_string(device_modes[DEVICE_CTRL_LASER]));

    lv_label_set_text(menu_tcp_btn, tcp_text);
    lv_label_set_text(menu_display_btn, display_text);
    lv_label_set_text(menu_exposure_btn, exposure_text);
    lv_label_set_text(menu_gain_btn, gain_text);
    lv_label_set_text(menu_usb_config_btn, usb_text);
    lv_label_set_text(menu_heater1_btn, heater1_text);
    lv_label_set_text(menu_heater2_btn, heater2_text);
    lv_label_set_text(menu_pump_btn, pump_text);
    lv_label_set_text(menu_laser_btn, laser_text);

    // 高亮当前选择的项目
    switch (menu_selected_item)
    {
    case MENU_ITEM_TCP: // TCP
        lv_obj_set_style_bg_color(menu_tcp_btn, lv_color_make(60, 60, 60), 0);
        lv_obj_set_style_bg_opa(menu_tcp_btn, LV_OPA_70, 0);
        if (is_tcp_available())
        {
            snprintf(tcp_text, sizeof(tcp_text), "> TCP: %s", tcp_enabled ? "ON" : "OFF");
        }
        else
        {
            snprintf(tcp_text, sizeof(tcp_text), "> TCP: N/A");
        }
        lv_label_set_text(menu_tcp_btn, tcp_text);
        break;
    case MENU_ITEM_DISPLAY: // DISPLAY
        lv_obj_set_style_bg_color(menu_display_btn, lv_color_make(60, 60, 60), 0);
        lv_obj_set_style_bg_opa(menu_display_btn, LV_OPA_70, 0);
        snprintf(display_text, sizeof(display_text), "> DISPLAY: %s", display_enabled ? "ON" : "OFF");
        lv_label_set_text(menu_display_btn, display_text);
        break;
    case MENU_ITEM_EXPOSURE: // EXPOSURE
        lv_obj_set_style_bg_color(menu_exposure_btn, lv_color_make(60, 60, 60), 0);
        lv_obj_set_style_bg_opa(menu_exposure_btn, LV_OPA_70, 0);
        if (in_adjustment_mode && adjustment_type == 0)
        {
            snprintf(exposure_text, sizeof(exposure_text), "> EXPOSURE: %d *", current_exposure);
        }
        else
        {
            snprintf(exposure_text, sizeof(exposure_text), "> EXPOSURE: %d", current_exposure);
        }
        lv_label_set_text(menu_exposure_btn, exposure_text);
        break;
    case MENU_ITEM_GAIN: // GAIN
        lv_obj_set_style_bg_color(menu_gain_btn, lv_color_make(60, 60, 60), 0);
        lv_obj_set_style_bg_opa(menu_gain_btn, LV_OPA_70, 0);
        if (in_adjustment_mode && adjustment_type == 1)
        {
            snprintf(gain_text, sizeof(gain_text), "> GAIN: %d *", current_gain);
        }
        else
        {
            snprintf(gain_text, sizeof(gain_text), "> GAIN: %d", current_gain);
        }
        lv_label_set_text(menu_gain_btn, gain_text);
        break;
    case MENU_ITEM_USB: // USB CONFIG
        lv_obj_set_style_bg_color(menu_usb_config_btn, lv_color_make(60, 60, 60), 0);
        lv_obj_set_style_bg_opa(menu_usb_config_btn, LV_OPA_70, 0);
        snprintf(usb_text, sizeof(usb_text), "> USB: %s", get_usb_mode_name(get_usb_mode()));
        lv_label_set_text(menu_usb_config_btn, usb_text);
        break;
    case MENU_ITEM_HEATER1:
        lv_obj_set_style_bg_color(menu_heater1_btn, lv_color_make(60, 60, 60), 0);
        lv_obj_set_style_bg_opa(menu_heater1_btn, LV_OPA_70, 0);
        snprintf(heater1_text, sizeof(heater1_text), "> HEATER1: %s", device_mode_to_string(device_modes[DEVICE_CTRL_HEATER1]));
        lv_label_set_text(menu_heater1_btn, heater1_text);
        break;
    case MENU_ITEM_HEATER2:
        lv_obj_set_style_bg_color(menu_heater2_btn, lv_color_make(60, 60, 60), 0);
        lv_obj_set_style_bg_opa(menu_heater2_btn, LV_OPA_70, 0);
        snprintf(heater2_text, sizeof(heater2_text), "> HEATER2: %s", device_mode_to_string(device_modes[DEVICE_CTRL_HEATER2]));
        lv_label_set_text(menu_heater2_btn, heater2_text);
        break;
    case MENU_ITEM_PUMP:
        lv_obj_set_style_bg_color(menu_pump_btn, lv_color_make(60, 60, 60), 0);
        lv_obj_set_style_bg_opa(menu_pump_btn, LV_OPA_70, 0);
        snprintf(pump_text, sizeof(pump_text), "> PUMP: %s", device_mode_to_string(device_modes[DEVICE_CTRL_PUMP]));
        lv_label_set_text(menu_pump_btn, pump_text);
        break;
    case MENU_ITEM_LASER:
        lv_obj_set_style_bg_color(menu_laser_btn, lv_color_make(60, 60, 60), 0);
        lv_obj_set_style_bg_opa(menu_laser_btn, LV_OPA_70, 0);
        snprintf(laser_text, sizeof(laser_text), "> LASER: %s", device_mode_to_string(device_modes[DEVICE_CTRL_LASER]));
        lv_label_set_text(menu_laser_btn, laser_text);
        break;
    }

    lv_obj_t *focused_obj = menu_item_object(menu_selected_item);
    if (focused_obj)
    {
        lv_obj_scroll_to_view(focused_obj, LV_ANIM_OFF);
    }
}

/**
 * @brief 菜单向上导航
 */
void menu_navigate_up(void)
{
    if (!menu_visible)
        return;

    if (in_adjustment_mode)
    {
        // 在调整模式中，向上增加数值
        if (adjustment_type == 0)
        {
            adjust_exposure_up();
        }
        else if (adjustment_type == 1)
        {
            adjust_gain_up();
        }
    }
    else
    {
        // 普通导航模式
        menu_selected_item--;
        if (menu_selected_item < 0)
        {
            menu_selected_item = MENU_ITEM_COUNT - 1;
        }
        update_menu_selection();
        printf("Menu navigation UP, selected item: %d\n", menu_selected_item);
    }
}

/**
 * @brief 菜单向下导航
 */
void menu_navigate_down(void)
{
    if (!menu_visible)
        return;

    if (in_adjustment_mode)
    {
        // 在调整模式中，向下减少数值
        if (adjustment_type == 0)
        {
            adjust_exposure_down();
        }
        else if (adjustment_type == 1)
        {
            adjust_gain_down();
        }
    }
    else
    {
        // 普通导航模式
        menu_selected_item++;
        if (menu_selected_item >= MENU_ITEM_COUNT)
        {
            menu_selected_item = 0;
        }
        update_menu_selection();
        printf("Menu navigation DOWN, selected item: %d\n", menu_selected_item);
    }
}

/**
 * @brief 确认菜单选择
 */
void menu_confirm_selection(void)
{
    if (!menu_visible)
        return;

    switch (menu_selected_item)
    {
    case MENU_ITEM_TCP: // TCP 选项
        // 检查TCP是否可用（只有RNDIS模式下才可用）
        if (!is_tcp_available() && !tcp_enabled)
        {
            printf("Menu: TCP not available in current USB mode (%s). Switch to RNDIS mode first.\n",
                   get_usb_mode_name(get_usb_mode()));
            break;
        }

        tcp_enabled = !tcp_enabled;
        printf("Menu: TCP transmission %s\n", tcp_enabled ? "ENABLED" : "DISABLED");

        if (tcp_enabled)
        {
            // 启动TCP传输 (中等优先级)
            if (server_fd < 0)
            {
                server_fd = create_server(DEFAULT_PORT);
                if (server_fd >= 0)
                {
                    pthread_attr_t tcp_attr;
                    struct sched_param tcp_param;
                    
                    pthread_attr_init(&tcp_attr);
                    pthread_attr_setdetachstate(&tcp_attr, PTHREAD_CREATE_JOINABLE);
                    
                    // 设置调度策略为FIFO，中等优先级
                    pthread_attr_setschedpolicy(&tcp_attr, SCHED_FIFO);
                    tcp_param.sched_priority = sched_get_priority_max(SCHED_FIFO) / 2;
                    pthread_attr_setschedparam(&tcp_attr, &tcp_param);
                    pthread_attr_setinheritsched(&tcp_attr, PTHREAD_EXPLICIT_SCHED);
                    
                    if (pthread_create(&tcp_thread_id, &tcp_attr, tcp_sender_thread, NULL) == 0)
                    {
                        printf("Menu: TCP server started successfully with priority %d\n", tcp_param.sched_priority);
                    }
                    else
                    {
                        printf("Menu: Failed to create TCP thread with priority, trying normal...\n");
                        // 如果优先级设置失败，尝试默认优先级
                        pthread_attr_destroy(&tcp_attr);
                        pthread_attr_init(&tcp_attr);
                        if (pthread_create(&tcp_thread_id, &tcp_attr, tcp_sender_thread, NULL) == 0)
                        {
                            printf("Menu: TCP server started successfully with normal priority\n");
                        }
                        else
                        {
                            printf("Menu: Failed to create TCP thread\n");
                            close(server_fd);
                            server_fd = -1;
                            tcp_enabled = 0;
                        }
                    }
                    
                    pthread_attr_destroy(&tcp_attr);
                }
                else
                {
                    printf("Menu: Failed to create TCP server\n");
                    tcp_enabled = 0;
                }
            }
        }
        else
        {
            // 停止TCP传输
            printf("Menu: Stopping TCP transmission...\n");

            // 关闭客户端连接
            if (client_connected)
            {
                shutdown(client_fd, SHUT_RDWR);
                close(client_fd);
                client_connected = 0;
                client_fd = -1;
            }

            // 关闭服务器socket
            if (server_fd >= 0)
            {
                shutdown(server_fd, SHUT_RDWR);
                close(server_fd);
                server_fd = -1;
            }
        }
        break;

    case MENU_ITEM_DISPLAY: // DISPLAY 选项
        display_enabled = !display_enabled;
        printf("Menu: Display %s (camera continues running)\n",
               display_enabled ? "ENABLED" : "DISABLED");

        // 使用LCD电源管理控制显示开关
        if (lcd_initialized)
        {
            if (display_enabled)
            {
                if (fbtft_lcd_power_on(&lcd_device) == 0)
                {
                    printf("LCD power turned ON\n");
                }
                else
                {
                    printf("Warning: Failed to turn LCD power ON\n");
                }
            }
            else
            {
                if (fbtft_lcd_power_off(&lcd_device) == 0)
                {
                    printf("LCD power turned OFF\n");
                }
                else
                {
                    printf("Warning: Failed to turn LCD power OFF\n");
                }
            }
        }
        else
        {
            // 降级到传统方式：控制图像显示
            if (img_canvas)
            {
                if (display_enabled)
                {
                    lv_obj_clear_flag(img_canvas, LV_OBJ_FLAG_HIDDEN);
                }
                else
                {
                    lv_obj_add_flag(img_canvas, LV_OBJ_FLAG_HIDDEN);
                }
            }
            printf("Warning: Using fallback display control (LCD power management not available)\n");
        }
        break;

    case MENU_ITEM_EXPOSURE: // EXPOSURE 选项
        if (in_adjustment_mode && adjustment_type == 0)
        {
            // 退出调整模式
            in_adjustment_mode = 0;
            printf("Menu: Exiting exposure adjustment mode\n");
        }
        else
        {
            // 进入调整模式
            in_adjustment_mode = 1;
            adjustment_type = 0; // 曝光调整
            printf("Menu: Entering exposure adjustment mode (UP/DOWN to adjust, KEY3 to exit)\n");
        }
        break;

    case MENU_ITEM_GAIN: // GAIN 选项
        if (in_adjustment_mode && adjustment_type == 1)
        {
            // 退出调整模式
            in_adjustment_mode = 0;
            printf("Menu: Exiting gain adjustment mode\n");
        }
        else
        {
            // 进入调整模式
            in_adjustment_mode = 1;
            adjustment_type = 1; // 增益调整
            printf("Menu: Entering gain adjustment mode (UP/DOWN to adjust, KEY3 to exit)\n");
        }
        break;

    case MENU_ITEM_USB: // USB CONFIG 选项
    {
        usb_mode_t current_mode = get_usb_mode();
        usb_mode_t next_mode = get_next_usb_mode(current_mode);

        printf("Menu: Switching USB mode from %s to %s\n",
               get_usb_mode_name(current_mode), get_usb_mode_name(next_mode));

        if (set_usb_mode(next_mode) == 0)
        {
            printf("Menu: USB mode changed to %s\n", get_usb_mode_name(next_mode));

            // 检查TCP是否仍然可用
            if (!is_tcp_available() && tcp_enabled)
            {
                tcp_enabled = 0;
                printf("Menu: TCP disabled due to USB mode change (TCP only available in RNDIS mode)\n");
            }
        }
        else
        {
            printf("Menu: Failed to change USB mode\n");
        }
    }
    break;

    case MENU_ITEM_HEATER1:
        cycle_device_mode(DEVICE_CTRL_HEATER1);
        break;

    case MENU_ITEM_HEATER2:
        cycle_device_mode(DEVICE_CTRL_HEATER2);
        break;

    case MENU_ITEM_PUMP:
        cycle_device_mode(DEVICE_CTRL_PUMP);
        break;

    case MENU_ITEM_LASER:
        cycle_device_mode(DEVICE_CTRL_LASER);
        break;
    }

    // 更新菜单显示
    if (menu_visible)
    {
        update_menu_selection();
    }

    update_activity_time();
}

/**
 * @brief 相机曝光设置菜单事件回调
 */
void menu_exposure_event_cb(lv_event_t *e)
{
    if (menu_visible && menu_selected_item == MENU_ITEM_TCP)
    {
        // 暂时禁用曝光调节，避免与其他操作冲突
        printf("Exposure adjustment temporarily disabled in menu\n");
        return;
    }

    // 曝光调节逻辑
    if (e->code == LV_EVENT_VALUE_CHANGED)
    {
        lv_obj_t *slider = e->target;
        int32_t new_value = lv_slider_get_value(slider);
        update_exposure_value(new_value);
    }
}

/**
 * @brief 相机增益设置菜单事件回调
 */
void menu_gain_event_cb(lv_event_t *e)
{
    if (menu_visible && menu_selected_item == MENU_ITEM_TCP)
    {
        // 暂时禁用增益调节，避免与其他操作冲突
        printf("Gain adjustment temporarily disabled in menu\n");
        return;
    }

    // 增益调节逻辑
    if (e->code == LV_EVENT_VALUE_CHANGED)
    {
        lv_obj_t *slider = e->target;
        int32_t new_value = lv_slider_get_value(slider);
        update_gain_value(new_value);
    }
}

/**
 * @brief 调高曝光值
 */
void adjust_exposure_up(void)
{
    int32_t new_value = current_exposure + exposure_step;
    if (new_value > exposure_max)
        new_value = exposure_max;
    exposure_value = new_value;

    update_exposure_value(new_value);
    printf("Exposure increased to: %d\n", current_exposure);
}

/**
 * @brief 调低曝光值
 */
void adjust_exposure_down(void)
{
    int32_t new_value = current_exposure - exposure_step;
    if (new_value < exposure_min)
        new_value = exposure_min;
    exposure_value = new_value;

    update_exposure_value(new_value);
    printf("Exposure decreased to: %d\n", current_exposure);
}

/**
 * @brief 调高增益值
 */
void adjust_gain_up(void)
{
    int32_t new_value = current_gain + gain_step;
    if (new_value > gain_max)
        new_value = gain_max;
    gain_value = new_value;

    update_gain_value(new_value);
    printf("Gain increased to: %d\n", current_gain);
}

/**
 * @brief 调低增益值
 */
void adjust_gain_down(void)
{
    int32_t new_value = current_gain - gain_step;
    if (new_value < gain_min)
        new_value = gain_min;
    gain_value = new_value;

    update_gain_value(new_value);
    printf("Gain decreased to: %d\n", current_gain);
}

/**
 * @brief 更新曝光值并应用到相机
 */
void update_exposure_value(int32_t new_value)
{
    if (subdev_handle < 0)
    {
        printf("Warning: Camera controls not initialized, cannot set exposure\n");
        return;
    }

    // 限制范围
    if (new_value < exposure_min)
        new_value = exposure_min;
    if (new_value > exposure_max)
        new_value = exposure_max;

    // 设置到硬件
    if (libmedia_set_exposure(subdev_handle, new_value) == 0)
    {
        current_exposure = new_value;
        printf("Exposure set to: %d\n", current_exposure);

        // 更新配置并保存到文件
        current_config.exposure = current_exposure;
        if (save_config_file(&current_config) == 0)
        {
            printf("Exposure value saved to config\n");
        }

        // 更新菜单显示
        if (menu_visible)
        {
            update_menu_selection();
        }
    }
    else
    {
        printf("Error: Failed to set exposure to %d\n", new_value);
    }
}

/**
 * @brief 更新增益值并应用到相机
 */
void update_gain_value(int32_t new_value)
{
    if (subdev_handle < 0)
    {
        printf("Warning: Camera controls not initialized, cannot set gain\n");
        return;
    }

    // 限制范围
    if (new_value < gain_min)
        new_value = gain_min;
    if (new_value > gain_max)
        new_value = gain_max;

    // 设置到硬件
    if (libmedia_set_gain(subdev_handle, new_value) == 0)
    {
        current_gain = new_value;
        printf("Gain set to: %d\n", current_gain);

        // 更新配置并保存到文件
        current_config.gain = current_gain;
        if (save_config_file(&current_config) == 0)
        {
            printf("Gain value saved to config\n");
        }

        // 更新菜单显示
        if (menu_visible)
        {
            update_menu_selection();
        }
    }
    else
    {
        printf("Error: Failed to set gain to %d\n", new_value);
    }
}

/**
 * @brief 初始化相机控制
 */
int init_camera_controls(void)
{
    // 打开子设备用于控制
    subdev_handle = libmedia_open_subdev("/dev/v4l-subdev2");
    if (subdev_handle < 0)
    {
        printf("Warning: Failed to open camera control subdevice, controls will not work\n");
        return -1;
    }

    // 获取当前曝光和增益值
    media_control_info_t info;

    // 获取曝光信息
    if (libmedia_get_control_info(subdev_handle, MEDIA_CTRL_EXPOSURE, &info) == 0)
    {
        exposure_min = info.min;
        exposure_max = info.max;
        current_exposure = info.current_value;
        printf("Camera control: Exposure range: %d-%d, current: %d\n",
               exposure_min, exposure_max, current_exposure);
    }
    else
    {
        printf("Warning: Failed to get exposure control info\n");
    }

    // 获取增益信息
    if (libmedia_get_control_info(subdev_handle, MEDIA_CTRL_ANALOGUE_GAIN, &info) == 0)
    {
        gain_min = info.min;
        gain_max = info.max;
        current_gain = info.current_value;
        printf("Camera control: Gain range: %d-%d, current: %d\n",
               gain_min, gain_max, current_gain);
    }
    else
    {
        printf("Warning: Failed to get gain control info\n");
    }

    printf("Camera controls initialized successfully\n");
    return 0;
}

/**
 * @brief 清理相机控制
 */
void cleanup_camera_controls(void)
{
    if (subdev_handle >= 0)
    {
        libmedia_close_subdev(subdev_handle);
        subdev_handle = -1;
        printf("Camera controls cleaned up\n");
    }
}

/**
 * @brief 创建图片保存目录
 */
int create_images_directory(void)
{
    // 创建根目录
    if (mkdir(CONFIG_IMAGE_PATH, 0755) != 0 && errno != EEXIST)
    {
        printf("Error: Failed to create %s directory: %s\n", CONFIG_IMAGE_PATH, strerror(errno));
        return -1;
    }

    return 0;
}

/**
 * @brief 生成照片文件名
 */
char *generate_photo_filename(void)
{
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);

    // 生成文件名字符串，包含分辨率信息
    static char filename[256];
    char timestamp[64];
    strftime(timestamp, sizeof(timestamp), "%H-%M-%S", tm_info);

    // 构建包含分辨率的文件名，使用 .bin 扩展名表示16位解包数据
    snprintf(filename, sizeof(filename), "%s/%04d-%02d-%02d_%s_%dx%d_16bit.bin",
             CONFIG_IMAGE_PATH,
             tm_info->tm_year + CONFIG_TIME_BASE_YEAR,
             tm_info->tm_mon + CONFIG_TIME_BASE_MONTH,
             tm_info->tm_mday + CONFIG_TIME_BASE_DAY,
             timestamp, camera_width, camera_height);

    return filename;
}

/**
 * @brief 捕获RAW格式照片
 */
int capture_raw_photo(void)
{
    if (!media_session)
    {
        printf("Error: Camera not initialized\n");
        return -1;
    }

    // 创建保存目录
    if (create_images_directory() != 0)
    {
        return -1;
    }

    // 生成文件名
    char *filename = generate_photo_filename();

    printf("Capturing photo to: %s\n", filename);
    printf("Target resolution: %dx%d (RAW10 format)\n", camera_width, camera_height);

    // 捕获一帧
    media_frame_t frame;
    int result = -1;
    bool is_frame_copy = false; // 标记是否为副本
    
    // 当TCP启用时，使用当前帧数据而不是重新捕获，避免与camera线程竞争
    if (tcp_enabled)
    {
        printf("TCP enabled - using current frame data to avoid resource conflict...\n");
        
        // 获取当前帧的副本，避免在TCP传输过程中被修改
        pthread_mutex_lock(&frame_mutex);
        if (current_frame.data && frame_available)
        {
            // 创建当前帧的副本
            frame.data = malloc(current_frame.size);
            if (frame.data)
            {
                memcpy(frame.data, current_frame.data, current_frame.size);
                frame.size = current_frame.size;
                frame.width = current_frame.width;
                frame.height = current_frame.height;
                frame.pixelformat = current_frame.pixelformat;
                result = 0;
                is_frame_copy = true; // 标记为副本
                printf("Using current frame: %zu bytes (%dx%d)\n", 
                       frame.size, frame.width, frame.height);
            }
            else
            {
                printf("Error: Failed to allocate memory for frame copy\n");
                result = -1;
            }
        }
        else
        {
            printf("Error: No current frame available\n");
            result = -1;
        }
        pthread_mutex_unlock(&frame_mutex);
    }
    else
    {
        // TCP未启用时，直接捕获新帧
        result = libmedia_session_capture_frame(media_session, &frame, 5000); // 5秒超时
    }

    if (result != 0)
    {
        printf("Error: Failed to capture frame for photo\n");
        return -1;
    }
    // 验证帧数据尺寸是否与设置的分辨率匹配
    size_t expected_size = camera_width * camera_height * 2; // RAW10 约2字节/像素
    if (frame.size != expected_size)
    {
        printf("Warning: Frame size mismatch - expected %zu bytes (%dx%d*2), got %zu bytes\n",
               expected_size, camera_width, camera_height, frame.size);
        printf("Continuing with actual frame size...\n");
    }
    else
    {
        printf("Frame size verified: %zu bytes (%dx%d RAW10)\n",
               frame.size, camera_width, camera_height);
    }

    // 分配缓冲区用于解包的像素数据
    size_t pixel_count = camera_width * camera_height;
    uint16_t *unpacked_pixels = malloc(pixel_count * sizeof(uint16_t));
    if (!unpacked_pixels)
    {
        printf("Error: Failed to allocate memory for unpacked pixels\n");
        if (is_frame_copy)
        {
            free(frame.data); // 释放副本内存
        }
        else
        {
            libmedia_session_release_frame(media_session, &frame);
        }
        return -1;
    }

    // 通过 unpack_sbggr10_image 解包RAW10数据
    printf("Unpacking RAW10 data (%zu bytes) to 16-bit pixels...\n", frame.size);
    int unpack_result = unpack_sbggr10_image((const uint8_t *)frame.data, frame.size,
                                             unpacked_pixels, camera_width, camera_height);

    if (unpack_result != 0)
    {
        printf("Error: Failed to unpack RAW10 data\n");
        free(unpacked_pixels);
        if (is_frame_copy)
        {
            free(frame.data); // 释放副本内存
        }
        else
        {
            libmedia_session_release_frame(media_session, &frame);
        }
        return -1;
    }

    printf("RAW10 data unpacked successfully to %zu 16-bit pixels\n", pixel_count);

    // 保存解包后的16位像素数据到文件
    FILE *file = fopen(filename, "wb");
    if (!file)
    {
        printf("Error: Failed to create file %s: %s\n", filename, strerror(errno));
        free(unpacked_pixels);
        if (is_frame_copy)
        {
            free(frame.data); // 释放副本内存
        }
        else
        {
            libmedia_session_release_frame(media_session, &frame);
        }
        return -1;
    }

    // 写入解包后的像素数据
    size_t data_size = pixel_count * sizeof(uint16_t);
    size_t written = fwrite(unpacked_pixels, 1, data_size, file);
    fclose(file);

    // 释放缓冲区和帧
    free(unpacked_pixels);
    if (is_frame_copy)
    {
        free(frame.data); // 释放副本内存
    }
    else
    {
        libmedia_session_release_frame(media_session, &frame);
    }

    if (written != data_size)
    {
        printf("Error: Incomplete write to %s (wrote %zu of %zu bytes)\n",
               filename, written, data_size);
        unlink(filename); // 删除不完整的文件
        return -1;
    }

    printf("Photo saved successfully: %s (%zu bytes, %dx%d 16-bit unpacked)\n",
           filename, written, camera_width, camera_height);

    // 显示简短的拍照成功提示
    if (info_label)
    {
        static char photo_msg[128];
        char *basename = strrchr(filename, '/');
        if (basename)
        {
            basename++; // 跳过 '/'
        }
        else
        {
            basename = filename;
        }
        snprintf(photo_msg, sizeof(photo_msg), "Photo: %s (%dx%d)", basename, camera_width, camera_height);
        lv_label_set_text(info_label, photo_msg);

        // 注意：这里简化处理，不使用定时器恢复信息显示
        // 用户可以通过其他操作来刷新信息显示
    }

    return 0;
}

/**
 * @brief 加载配置文件
 */
int load_config_file(mxcamera_config_t *config)
{
    if (!config)
        return -1;

    // 尝试打开配置文件
    FILE *file = fopen(CONFIG_FILE_PATH, "r");
    if (!file)
    {
        printf("Warning: Could not open config file %s: %s\n", CONFIG_FILE_PATH, strerror(errno));
        return -1;
    }

    char line[CONFIG_MAX_LINE_LENGTH];
    char key[CONFIG_MAX_KEY_LENGTH];
    char value[CONFIG_MAX_VALUE_LENGTH];

    // 逐行读取配置
    while (fgets(line, sizeof(line), file))
    {
        // 去除行尾换行符
        line[strcspn(line, "\r\n")] = 0;

        // 跳过空行和注释
        if (line[0] == '\0' || line[0] == '#')
        {
            continue;
        }

        // 解析键值对
        if (parse_config_line(line, key, value) == 0)
        {
            // 根据键设置配置项
            if (strcmp(key, "camera_width") == 0)
            {
                config->camera_width = atoi(value);
            }
            else if (strcmp(key, "camera_height") == 0)
            {
                config->camera_height = atoi(value);
            }
            else if (strcmp(key, "crop_top") == 0)
            {
                config->crop_top = atoi(value);
            }
            else if (strcmp(key, "crop_left") == 0)
            {
                config->crop_left = atoi(value);
            }
            else if (strcmp(key, "exposure") == 0)
            {
                config->exposure = atoi(value);
            }
            else if (strcmp(key, "gain") == 0)
            {
                config->gain = atoi(value);
            }
            else if (strcmp(key, "exposure_step") == 0)
            {
                config->exposure_step = atoi(value);
            }
            else if (strcmp(key, "gain_step") == 0)
            {
                config->gain_step = atoi(value);
            }
        }
    }

    fclose(file);
    return 0;
}

/**
 * @brief 保存配置文件
 */
int save_config_file(const mxcamera_config_t *config)
{
    if (!config)
        return -1;

    // 创建目录（如果不存在）
    char dir_path[] = "/root/Workspace";
    mkdir(dir_path, 0755);

    // 尝试打开配置文件
    FILE *file = fopen(CONFIG_FILE_PATH, "w");
    if (!file)
    {
        printf("Error: Could not open config file %s for writing: %s\n", CONFIG_FILE_PATH, strerror(errno));
        return -1;
    }

    // 写入 TOML 格式的配置文件
    fprintf(file, "# mxCamera Configuration File\n");
    fprintf(file, "# This file is automatically generated and updated by mxCamera\n");
    fprintf(file, "# Last updated: %s\n", __DATE__ " " __TIME__);
    fprintf(file, "\n");
    fprintf(file, "[camera]\n");
    fprintf(file, "camera_width = %d\n", config->camera_width);
    fprintf(file, "camera_height = %d\n", config->camera_height);
    fprintf(file, "crop_top = %d\n", config->crop_top);
    fprintf(file, "crop_left = %d\n", config->crop_left);
    fprintf(file, "\n");
    fprintf(file, "[controls]\n");
    fprintf(file, "exposure = %d\n", config->exposure);
    fprintf(file, "gain = %d\n", config->gain);
    fprintf(file, "exposure_step = %d\n", config->exposure_step);
    fprintf(file, "gain_step = %d\n", config->gain_step);

    fclose(file);
    printf("Configuration saved to %s\n", CONFIG_FILE_PATH);
    return 0;
}

/**
 * @brief 应用配置
 */
void apply_config(const mxcamera_config_t *config)
{
    if (!config)
        return;

    // 应用摄像头配置
    camera_width = config->camera_width;
    camera_height = config->camera_height;
    
    // 应用裁剪参数
    crop_top = config->crop_top;
    crop_left = config->crop_left;

    // 应用曝光和增益
    current_exposure = config->exposure;
    current_gain = config->gain;
    exposure_step = config->exposure_step;
    gain_step = config->gain_step;

    // 更新菜单显示
    if (menu_visible)
    {
        update_menu_selection();
    }

    printf("Config applied: %dx%d, crop_top: %d, crop_left: %d, device: %s, exposure: %d, gain: %d\n",
           camera_width, camera_height, crop_top, crop_left, DEFAULT_CAMERA_DEVICE, current_exposure, current_gain);
}

/**
 * @brief 初始化默认配置
 */
void init_default_config(mxcamera_config_t *config)
{
    if (!config)
        return;

    // 设置默认值
    config->camera_width = DEFAULT_CAMERA_WIDTH;
    config->camera_height = DEFAULT_CAMERA_HEIGHT;
    config->crop_top = 0;        // 默认不裁剪
    config->crop_left = 0;       // 默认不裁剪
    config->exposure = 128;
    config->gain = 128;
    config->exposure_step = 16;
    config->gain_step = 32;
}

/**
 * @brief 去除字符串首尾空白字符
 */
char *trim_whitespace(char *str)
{
    char *end;

    // 去除开头空白
    while (isspace((unsigned char)*str))
        str++;

    // 全部为空白
    if (*str == 0)
        return str;

    // 去除结尾空白
    end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end))
        end--;

    // 结尾加上空字符
    *(end + 1) = 0;

    return str;
}

/**
 * @brief 解析配置文件中的一行
 */
int parse_config_line(const char *line, char *key, char *value)
{
    const char *delim = "=";
    char *eq_pos = strstr(line, delim);

    if (!eq_pos)
    {
        return -1; // 没有找到分隔符
    }

    // 提取键
    size_t key_len = eq_pos - line;
    strncpy(key, line, key_len);
    key[key_len] = '\0';

    // 提取值
    char *val_pos = eq_pos + strlen(delim);
    strncpy(value, val_pos, CONFIG_MAX_VALUE_LENGTH - 1);
    value[CONFIG_MAX_VALUE_LENGTH - 1] = '\0';

    // 去除首尾空白
    char *trimmed_key = trim_whitespace(key);
    char *trimmed_value = trim_whitespace(value);

    // 将处理后的结果复制回原变量
    if (trimmed_key != key)
    {
        memmove(key, trimmed_key, strlen(trimmed_key) + 1);
    }
    if (trimmed_value != value)
    {
        memmove(value, trimmed_value, strlen(trimmed_value) + 1);
    }

    // 处理带引号的字符串值
    size_t value_len = strlen(value);
    if (value_len >= 2 && value[0] == '"' && value[value_len - 1] == '"')
    {
        // 去除引号
        memmove(value, value + 1, value_len - 2);
        value[value_len - 2] = '\0';

        // 去除引号后再次修剪空白
        char *final_trimmed = trim_whitespace(value);
        if (final_trimmed != value)
        {
            memmove(value, final_trimmed, strlen(final_trimmed) + 1);
        }
    }

    return 0;
}
