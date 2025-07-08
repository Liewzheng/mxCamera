#ifndef _MXCAMERA_H_
#define _MXCAMERA_H_

/**
 * @file mxCamera.h
 * @brief mxCamera 公共头文件
 * 
 * 包含所有模块的公共定义、结构体和函数声明
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
#include <sys/stat.h>
#include <sys/types.h>
#include <ctype.h>
#include <stdint.h>

// 库头文件
#include <gpio.h>
#include <media.h>
#include "DEV_Config.h"
#include "lv_drivers/display/fbdev.h"
#include "lvgl/lvgl.h"
#include "fbtft_lcd.h"

// TCP 传输相关头文件
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/select.h>

// // ============================================================================
// // 系统配置常量
// // ============================================================================

// #define DISP_BUF_SIZE (320 * 240)

// // 摄像头配置 (默认值，可通过命令行参数覆盖)
// #define DEFAULT_CAMERA_WIDTH 1920
// #define DEFAULT_CAMERA_HEIGHT 1080
// #define CAMERA_PIXELFORMAT V4L2_PIX_FMT_SBGGR10
// #define DEFAULT_CAMERA_DEVICE "/dev/video0"
// #define BUFFER_COUNT 4

// // 显示配置 (横屏模式)
// #define DISPLAY_WIDTH 320
// #define DISPLAY_HEIGHT 240

// // TCP 传输配置
// #define DEFAULT_PORT 8888
// #define DEFAULT_SERVER_IP "172.32.0.93"
// #define HEADER_SIZE 32
// #define CHUNK_SIZE 65536

// // 帧率统计配置
// #undef FPS_UPDATE_INTERVAL
// #define FPS_UPDATE_INTERVAL 1000000 // 1秒 (微秒)

// // 按键配置 (使用 DEV_Config.h 中已定义的 PIN)
// // KEY0_PIN, KEY1_PIN, KEY2_PIN, KEY3_PIN 已在 DEV_Config.h 中定义

// // GPIO 按键宏定义
// #define GET_KEY0 DEV_Digital_Read(KEY0_PIN)
// #define GET_KEY1 DEV_Digital_Read(KEY1_PIN)
// #define GET_KEY2 DEV_Digital_Read(KEY2_PIN)
// #define GET_KEY3 DEV_Digital_Read(KEY3_PIN)

// // 配置文件路径
// #define CONFIG_FILE_PATH "/root/Workspace/mxCamera_config.toml"

// // ============================================================================
// // 数据结构定义
// // ============================================================================

/**
 * @struct frame_header
 * @brief TCP传输数据帧头部结构
 */
struct frame_header {
    uint32_t magic;       /**< 魔数标识：0xDEADBEEF */
    uint32_t frame_id;    /**< 帧序号 */
    uint32_t width;       /**< 图像宽度 */
    uint32_t height;      /**< 图像高度 */
    uint32_t pixfmt;      /**< 像素格式 */
    uint32_t size;        /**< 数据大小 */
    uint64_t timestamp;   /**< 时间戳 */
    uint32_t reserved[2]; /**< 保留字段 */
} __attribute__((packed));

/**
 * @struct mxcamera_config_t
 * @brief mxCamera 配置结构体
 */
typedef struct {
    // 摄像头配置
    int camera_width;
    int camera_height;
    
    // 控制参数
    int exposure;
    int gain;
    int exposure_step;
    int gain_step;
} mxcamera_config_t;

// /**
//  * @struct camera_status_t
//  * @brief 摄像头状态结构体
//  */
// typedef struct {
//     int is_initialized;      /**< 是否已初始化 */
//     int is_capturing;        /**< 是否正在采集 */
//     int is_paused;           /**< 是否暂停 */
//     int32_t current_exposure; /**< 当前曝光值 */
//     int32_t current_gain;     /**< 当前增益值 */
//     uint32_t frame_count;     /**< 总帧数 */
//     float current_fps;        /**< 当前帧率 */
//     int width;               /**< 图像宽度 */
//     int height;              /**< 图像高度 */
// } camera_status_t;

// // ============================================================================
// // 全局变量声明 (外部可访问)
// // ============================================================================

// // 系统状态
// extern volatile int exit_flag;
// extern volatile int camera_paused;
// extern volatile int display_enabled;
// extern volatile int tcp_enabled;
// extern volatile int screen_on;
// extern volatile int menu_visible;
// extern volatile int menu_selected_item;
// extern volatile int in_adjustment_mode;
// extern volatile int adjustment_type;

// // 摄像头配置
// extern int camera_width;
// extern int camera_height;
// extern int current_img_width;
// extern int current_img_height;

// // TCP 配置
// extern int tcp_port;
// extern char tcp_server_ip[32];

// // 当前控制值
// extern int32_t current_exposure;
// extern int32_t current_gain;
// extern int32_t exposure_step;
// extern int32_t gain_step;
// extern int32_t exposure_min;
// extern int32_t exposure_max;
// extern int32_t gain_min;
// extern int32_t gain_max;

// // 摄像头控制
// extern int subdev_handle;

// // TCP 连接状态
// extern volatile int client_connected;
// extern int server_fd;
// extern int client_fd;
// extern pthread_t tcp_thread_id;

// // 帧统计
// extern uint32_t frame_count;
// extern struct timeval last_fps_time;
// extern float current_fps;
// extern int frame_available;

// // LVGL 对象
// extern lv_obj_t* img_canvas;
// extern lv_obj_t* info_label;
// extern lv_obj_t* time_label;
// extern lv_obj_t* menu_panel;
// extern lv_obj_t* menu_tcp_btn;
// extern lv_obj_t* menu_display_btn;
// extern lv_obj_t* menu_exposure_btn;
// extern lv_obj_t* menu_gain_btn;

// // 配置
// extern mxcamera_config_t current_config;
// extern int config_loaded;

// // 媒体会话
// extern media_session_t* media_session;
// extern media_frame_t current_frame;
// extern pthread_mutex_t frame_mutex;
// extern pthread_cond_t frame_ready;

// // LCD设备
// extern fbtft_lcd_t lcd_device;
// extern int lcd_initialized;

// // 时间统计
// extern struct timeval last_activity_time;
// extern struct timeval last_time_update;

// ============================================================================
// UI 模块函数声明 (ui.c)
// ============================================================================

void update_fps(void);
void update_image_display(void);
void init_lvgl_ui(void);
void update_time_display(void);
void show_settings_menu(void);
void hide_settings_menu(void);
void update_menu_selection(void);
void menu_navigate_up(void);
void menu_navigate_down(void);
void menu_confirm_selection(void);
void menu_tcp_event_cb(lv_event_t* e);
void menu_display_event_cb(lv_event_t* e);
void menu_close_event_cb(lv_event_t* e);


// 信号处理和系统配置
void signal_handler(int sig);
void check_display_config(void);
void print_usage(const char* program_name);
int parse_arguments(int argc, char* argv[]);

// 图像处理和缩放
void calculate_scaled_size(int src_width, int src_height, int* dst_width, int* dst_height);
void unpack_sbggr10_scalar(const uint8_t raw_bytes[5], uint16_t pixels[4]);
int unpack_sbggr10_image(const uint8_t *raw_data, size_t raw_size, 
                               uint16_t *output_pixels, int width, int height);
void scale_pixels(const uint16_t* src_pixels, int src_width, int src_height,
                        uint16_t* dst_pixels, int dst_width, int dst_height);
void convert_pixels_to_rgb565(const uint16_t* pixels, uint16_t* rgb565_data,
                                    int width, int height);
int landscape_image_fit(const uint16_t* src_buffer, int src_width, int src_height, 
                              uint16_t* dst_buffer);

// 相机控制相关函数
void menu_exposure_event_cb(lv_event_t* e);
void menu_gain_event_cb(lv_event_t* e);
void adjust_exposure_up(void);
void adjust_exposure_down(void);
void adjust_gain_up(void);
void adjust_gain_down(void);
int init_camera_controls(void);
void cleanup_camera_controls(void);
void update_exposure_value(int32_t new_value);
void update_gain_value(int32_t new_value);

// 配置文件管理
int load_config_file(mxcamera_config_t* config);
int save_config_file(const mxcamera_config_t* config);
void apply_config(const mxcamera_config_t* config);
void init_default_config(mxcamera_config_t* config);
char* trim_whitespace(char* str);
int parse_config_line(const char* line, char* key, char* value);

// 拍照功能
int capture_raw_photo(void);
int create_images_directory(void);
char* generate_photo_filename(void);

// 系统资源监控
float get_cpu_usage(void);
float get_memory_usage(void);
void update_system_info(void);

// 线程和输入处理
void* camera_thread(void* arg);
void handle_keys(void);

// 屏幕控制
void turn_screen_off(void);
void turn_screen_on(void);
void check_screen_timeout(void);
void update_activity_time(void);

// 资源清理
void cleanup_image_buffers(void);

// TCP 传输相关函数
uint64_t get_time_ns(void);
int create_server(int port);
int send_frame(int fd, void* data, size_t size, uint32_t frame_id, uint64_t timestamp);
void* tcp_sender_thread(void* arg);
// ============================================================================
// I2C 模块函数声明 (i2c.c)
// ============================================================================

int init_ina219(void);
void cleanup_ina219(void);
int read_ina219_data(float* bus_voltage, float* shunt_voltage, float* current, float* power);
int update_battery_status(void);
float get_battery_percentage(void);
float get_battery_voltage(void);
float get_battery_current(void);
float get_battery_power(void);
int is_ina219_initialized(void);
int analyze_system_health(float voltage, float current, float power);
void print_battery_detailed_status(void);

#endif /* _MXCAMERA_H_ */
