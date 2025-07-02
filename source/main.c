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

// 库头文件
#include <gpio.h>
#include <media.h>
#include "DEV_Config.h"
#include "lv_drivers/display/fbdev.h"
#include "lvgl/lvgl.h"
#include "fbtft_lcd.h"

// TCP 传输相关数据结构 (参考 media_usb)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/select.h>

/**
 * @struct frame_header
 * @brief 数据帧头部结构
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

// ============================================================================
// 系统配置常量
// ============================================================================

#define DISP_BUF_SIZE (320 * 240)

// 摄像头配置
#define CAMERA_WIDTH 1920
#define CAMERA_HEIGHT 1080
#define CAMERA_PIXELFORMAT V4L2_PIX_FMT_SBGGR10
#define CAMERA_DEVICE "/dev/video0"
#define BUFFER_COUNT 4

// 显示配置 (横屏模式)
#define DISPLAY_WIDTH 320
#define DISPLAY_HEIGHT 240

// TCP 传输配置 (参考 media_usb)
#define DEFAULT_PORT 8888
#define DEFAULT_SERVER_IP "172.32.0.93"
#define HEADER_SIZE 32
#define CHUNK_SIZE 65536

// 动态图像尺寸 (根据摄像头宽高比计算)
static int current_img_width = 320;
static int current_img_height = 240;

// 帧率统计配置 (重新定义以避免冲突)
#undef FPS_UPDATE_INTERVAL
#define FPS_UPDATE_INTERVAL 1000000 // 1秒 (微秒)

// ============================================================================
// 函数声明
// ============================================================================

// 信号处理和系统配置
static void signal_handler(int sig);
static void check_display_config(void);

// 图像处理和缩放
static void calculate_scaled_size(int src_width, int src_height, int* dst_width, int* dst_height);
static void unpack_sbggr10_scalar(const uint8_t raw_bytes[5], uint16_t pixels[4]);
static int unpack_sbggr10_image(const uint8_t *raw_data, size_t raw_size, 
                               uint16_t *output_pixels, int width, int height);
static void scale_pixels(const uint16_t* src_pixels, int src_width, int src_height,
                        uint16_t* dst_pixels, int dst_width, int dst_height);
static void convert_pixels_to_rgb565(const uint16_t* pixels, uint16_t* rgb565_data,
                                    int width, int height);
static int landscape_image_fit(const uint16_t* src_buffer, int src_width, int src_height, 
                              uint16_t* dst_buffer);

// 显示和UI更新
static void update_fps(void);
static void update_image_display(void);
static void init_lvgl_ui(void);

// 系统资源监控
static float get_cpu_usage(void);
static float get_memory_usage(void);
static void update_system_info(void);

// 线程和输入处理
static void* camera_thread(void* arg);
static void handle_keys(void);

// 屏幕控制
static void turn_screen_off(void);
static void turn_screen_on(void);
static void check_screen_timeout(void);
static void update_activity_time(void);

// TCP 传输相关函数
static uint64_t get_time_ns(void);
static int create_server(int port);
static int send_frame(int fd, void* data, size_t size, uint32_t frame_id, uint64_t timestamp);
static void* tcp_sender_thread(void* arg);

// ============================================================================
// 全局变量
// ============================================================================

// 系统状态
static volatile int exit_flag = 0;
static volatile int camera_paused = 0;
static volatile int tcp_enabled = 0;
static volatile int screen_on = 1;          // 屏幕开关状态
static struct timeval last_activity_time;  // 最后活动时间

// TCP 传输状态
static volatile int client_connected = 0;
static int server_fd = -1;
static int client_fd = -1;
static pthread_t tcp_thread_id;

// 媒体会话
static media_session_t* media_session = NULL;

// LVGL 对象
static lv_obj_t* img_canvas = NULL;
static lv_obj_t* status_label = NULL;
static lv_obj_t* info_label = NULL;
static lv_obj_t* tcp_label = NULL;

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

// ============================================================================
// 工具函数
// ============================================================================

/**
 * @brief 信号处理函数
 */
static void signal_handler(int sig) {
    printf("\nReceived signal %d, cleaning up...\n", sig);
    exit_flag = 1;
    tcp_enabled = 0; // 停止TCP传输
    
    // 强制刷新输出缓冲区
    fflush(stdout);
    fflush(stderr);
    
    // 关闭TCP连接
    if (client_connected && client_fd >= 0) {
        shutdown(client_fd, SHUT_RDWR);
        close(client_fd);
        client_connected = 0;
        client_fd = -1;
    }
    if (server_fd >= 0) {
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

/**
 * @brief 检查和配置显示设备
 */
static void check_display_config(void) {
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
static uint64_t get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/**
 * @brief 创建TCP服务器
 */
static int create_server(int port) {
    int fd;
    struct sockaddr_in addr;
    int opt = 1;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket failed");
        return -1;
    }

    // 设置 SO_REUSEADDR 选项，允许重用地址
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt SO_REUSEADDR failed");
        close(fd);
        return -1;
    }
    
    // 设置 SO_REUSEPORT 选项（如果支持），允许重用端口
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) < 0) {
        // 某些系统可能不支持 SO_REUSEPORT，这里只打印警告
        printf("Warning: SO_REUSEPORT not supported\n");
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr(DEFAULT_SERVER_IP);
    addr.sin_port = htons(port);

    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind failed");
        close(fd);
        return -1;
    }

    if (listen(fd, 1) < 0) {
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
static int send_frame(int fd, void* data, size_t size, uint32_t frame_id, uint64_t timestamp) {
    struct frame_header header = {
        .magic = 0xDEADBEEF,
        .frame_id = frame_id,
        .width = CAMERA_WIDTH,
        .height = CAMERA_HEIGHT,
        .pixfmt = CAMERA_PIXELFORMAT,
        .size = size,
        .timestamp = timestamp,
        .reserved = {0, 0}
    };

    // 发送帧头
    if (send(fd, &header, sizeof(header), MSG_NOSIGNAL) != sizeof(header)) {
        return -1;
    }

    // 分块发送数据
    size_t sent = 0;
    uint8_t* ptr = (uint8_t*)data;

    while (sent < size && !exit_flag) {
        size_t to_send = (size - sent) > CHUNK_SIZE ? CHUNK_SIZE : (size - sent);
        ssize_t result = send(fd, ptr + sent, to_send, MSG_NOSIGNAL);

        if (result <= 0) {
            return -1;
        }

        sent += result;
    }

    return 0;
}

/**
 * @brief TCP数据发送线程函数
 */
static void* tcp_sender_thread(void* arg) {
    (void)arg; // 避免未使用参数警告
    printf("TCP sender thread started\n");
    static uint32_t tcp_frame_counter = 0;

    while (!exit_flag && tcp_enabled) {
        // 等待客户端连接
        if (!client_connected && tcp_enabled && server_fd >= 0) {
            struct sockaddr_in client_addr;
            socklen_t client_len = sizeof(client_addr);

            printf("Waiting for TCP client connection...\n");
            
            // 设置 socket 为非阻塞模式，避免 accept 无限阻塞
            fd_set readfds;
            struct timeval timeout;
            
            FD_ZERO(&readfds);
            FD_SET(server_fd, &readfds);
            timeout.tv_sec = 1;  // 1秒超时
            timeout.tv_usec = 0;
            
            int select_result = select(server_fd + 1, &readfds, NULL, NULL, &timeout);
            
            if (select_result > 0 && FD_ISSET(server_fd, &readfds)) {
                client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
                
                if (client_fd >= 0) {
                    printf("TCP Client connected from %s\n", inet_ntoa(client_addr.sin_addr));
                    client_connected = 1;
                } else {
                    if (tcp_enabled && !exit_flag) {
                        perror("accept failed");
                    }
                }
            } else if (select_result < 0 && tcp_enabled && !exit_flag) {
                perror("select failed");
                break;
            }
            
            // 检查是否需要退出
            if (!tcp_enabled || exit_flag) {
                break;
            }
            continue;
        }

        // 等待新帧数据
        pthread_mutex_lock(&frame_mutex);
        while (current_frame.data == NULL && !exit_flag && tcp_enabled) {
            struct timespec timeout;
            clock_gettime(CLOCK_REALTIME, &timeout);
            timeout.tv_sec += 1; // 1秒超时
            pthread_cond_timedwait(&frame_ready, &frame_mutex, &timeout);
        }

        if (current_frame.data && !exit_flag && tcp_enabled && client_connected) {
            // 发送原始RAW10帧数据
            uint64_t timestamp = get_time_ns();
            if (send_frame(client_fd, current_frame.data, current_frame.size,
                          tcp_frame_counter++, timestamp) < 0) {
                printf("TCP Client disconnected (frame %d)\n", tcp_frame_counter);
                close(client_fd);
                client_connected = 0;
            }
        }

        pthread_mutex_unlock(&frame_mutex);
        
        // 如果TCP被禁用，退出循环
        if (!tcp_enabled) {
            break;
        }
        
        usleep(1000); // 1ms
    }

    // 清理TCP连接
    if (client_connected && client_fd >= 0) {
        shutdown(client_fd, SHUT_RDWR);
        close(client_fd);
        client_connected = 0;
        client_fd = -1;
    }

    printf("TCP sender thread terminated\n");
    return NULL;
}

/**
 * @brief 更新最后活动时间
 */
static void update_activity_time(void) {
    gettimeofday(&last_activity_time, NULL);
}

/**
 * @brief 关闭屏幕
 */
static void turn_screen_off(void) {
    if (!screen_on) return;
    
    printf("Turning screen OFF (auto-sleep after 5s pause)\n");
    screen_on = 0;
    
    // 设置屏幕亮度为0或关闭背光
    system("echo 0 > /sys/class/backlight/*/brightness 2>/dev/null");
    
    // 隐藏所有UI元素
    if (img_canvas) lv_obj_add_flag(img_canvas, LV_OBJ_FLAG_HIDDEN);
    if (info_label) lv_obj_add_flag(info_label, LV_OBJ_FLAG_HIDDEN);
    if (status_label) lv_obj_add_flag(status_label, LV_OBJ_FLAG_HIDDEN);
    if (tcp_label) lv_obj_add_flag(tcp_label, LV_OBJ_FLAG_HIDDEN);
    
    // 清空屏幕为黑色
    lv_obj_t* scr = lv_disp_get_scr_act(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
}

/**
 * @brief 打开屏幕
 */
static void turn_screen_on(void) {
    if (screen_on) return;
    
    printf("Turning screen ON (key wake-up)\n");
    screen_on = 1;
    
    // 恢复屏幕亮度
    system("echo 255 > /sys/class/backlight/*/brightness 2>/dev/null");
    
    // 显示所有UI元素
    if (img_canvas) lv_obj_clear_flag(img_canvas, LV_OBJ_FLAG_HIDDEN);
    if (info_label) lv_obj_clear_flag(info_label, LV_OBJ_FLAG_HIDDEN);
    if (status_label) lv_obj_clear_flag(status_label, LV_OBJ_FLAG_HIDDEN);
    if (tcp_label) lv_obj_clear_flag(tcp_label, LV_OBJ_FLAG_HIDDEN);
    
    // 更新活动时间
    update_activity_time();
}

/**
 * @brief 检查屏幕超时并自动关闭
 */
static void check_screen_timeout(void) {
    if (!screen_on || !camera_paused) return;
    
    struct timeval current_time;
    gettimeofday(&current_time, NULL);
    
    long time_since_activity = (current_time.tv_sec - last_activity_time.tv_sec) * 1000000 +
                              (current_time.tv_usec - last_activity_time.tv_usec);
    
    // 5秒超时 (5,000,000 微秒)
    if (time_since_activity >= 5000000) {
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
static void calculate_scaled_size(int src_width, int src_height, int* dst_width, int* dst_height) {
    if (src_width <= 0 || src_height <= 0) {
        *dst_width = DISPLAY_WIDTH;
        *dst_height = DISPLAY_HEIGHT;
        return;
    }
    
    // 计算宽高比
    float aspect_ratio = (float)src_height / (float)src_width;
    
    // 宽度固定为屏幕宽度320，高度根据宽高比计算
    *dst_width = 320;  // 强制使用320像素宽度
    *dst_height = (int)(320 * aspect_ratio);
    
    // 确保高度不超过屏幕高度240
    if (*dst_height > 240) {
        *dst_height = 240;
        *dst_width = (int)(240 / aspect_ratio);
    }
    
    // 确保最小尺寸
    if (*dst_width < 160) *dst_width = 160;
    if (*dst_height < 120) *dst_height = 120;
    
    printf("Image scaling: %dx%d -> %dx%d (aspect ratio: %.3f)\n", 
           src_width, src_height, *dst_width, *dst_height, (double)aspect_ratio);
}

/**
 * @brief 计算帧率
 */
static void update_fps(void) {
    struct timeval current_time;
    gettimeofday(&current_time, NULL);
    
    long time_diff = (current_time.tv_sec - last_fps_time.tv_sec) * 1000000 +
                     (current_time.tv_usec - last_fps_time.tv_usec);
    
    if (time_diff >= FPS_UPDATE_INTERVAL) {
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
static float get_cpu_usage(void) {
    static unsigned long long last_total = 0, last_idle = 0;
    unsigned long long total, idle, user, nice, system, iowait, irq, softirq, steal;
    
    FILE *fp = fopen("/proc/stat", "r");
    if (!fp) {
        return -1.0f;
    }
    
    if (fscanf(fp, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
               &user, &nice, &system, &idle, &iowait, &irq, &softirq, &steal) != 8) {
        fclose(fp);
        return -1.0f;
    }
    fclose(fp);
    
    total = user + nice + system + idle + iowait + irq + softirq + steal;
    
    if (last_total == 0) {
        // 第一次调用，保存当前值
        last_total = total;
        last_idle = idle;
        return 0.0f;
    }
    
    unsigned long long total_diff = total - last_total;
    unsigned long long idle_diff = idle - last_idle;
    
    float cpu_usage = 0.0f;
    if (total_diff > 0) {
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
static float get_memory_usage(void) {
    FILE *fp = fopen("/proc/meminfo", "r");
    if (!fp) {
        return -1.0f;
    }
    
    unsigned long mem_total = 0, mem_free = 0, buffers = 0, cached = 0;
    char line[256];
    
    while (fgets(line, sizeof(line), fp)) {
        if (sscanf(line, "MemTotal: %lu kB", &mem_total) == 1) continue;
        if (sscanf(line, "MemFree: %lu kB", &mem_free) == 1) continue;
        if (sscanf(line, "Buffers: %lu kB", &buffers) == 1) continue;
        if (sscanf(line, "Cached: %lu kB", &cached) == 1) continue;
    }
    fclose(fp);
    
    if (mem_total == 0) {
        return -1.0f;
    }
    
    unsigned long mem_used = mem_total - mem_free - buffers - cached;
    return 100.0f * (float)mem_used / (float)mem_total;
}

/**
 * @brief 更新系统信息显示（合并图像大小、帧率、CPU和内存占用）
 */
static void update_system_info(void) {
    if (!info_label) return;
    
    static struct timeval last_update = {0};
    struct timeval current_time;
    gettimeofday(&current_time, NULL);
    
    // 每500ms更新一次系统信息（提高更新频率以显示实时FPS）
    long time_diff = (current_time.tv_sec - last_update.tv_sec) * 1000000 +
                     (current_time.tv_usec - last_update.tv_usec);
    
    if (time_diff >= 500000) { // 0.5秒
        float cpu_usage = get_cpu_usage();
        float mem_usage = get_memory_usage();
        
        char info_text[64];
        // 格式：图像大小 帧率 CPU占用率% 内存占用率%
        // 例如：1920x1080 30.4 98% 70%
        snprintf(info_text, sizeof(info_text), 
                "%dx%d  %.1fFPS  %.0f%%  %.0f%%", 
                CAMERA_WIDTH, CAMERA_HEIGHT,
                (double)current_fps, 
                (double)(cpu_usage >= 0 ? cpu_usage : 0),
                (double)(mem_usage >= 0 ? mem_usage : 0));
        
        lv_label_set_text(info_label, info_text);
        last_update = current_time;
    }
}

/**
 * @brief SBGGR10格式数据解包（标量版本）- 参考v4l2_bench实现
 * @param raw_bytes 5字节的RAW10数据（包含4个像素）
 * @param pixels 输出的4个16位像素值
 */
static void unpack_sbggr10_scalar(const uint8_t raw_bytes[5], uint16_t pixels[4]) {
    // 重构40位数据
    uint64_t combined = ((uint64_t)raw_bytes[4] << 32) |
                       ((uint64_t)raw_bytes[3] << 24) |
                       ((uint64_t)raw_bytes[2] << 16) |
                       ((uint64_t)raw_bytes[1] << 8)  |
                        (uint64_t)raw_bytes[0];
    
    // 提取4个10位像素值（小端序，从低位开始）
    pixels[0] = (uint16_t)((combined >>  0) & 0x3FF);
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
static int unpack_sbggr10_image(const uint8_t *raw_data, size_t raw_size, 
                               uint16_t *output_pixels, int width, int height) {
    if (!raw_data || !output_pixels || raw_size == 0) {
        return -1;
    }
    
    // 验证数据大小（必须是5的倍数）
    if (raw_size % 5 != 0) {
        printf("Error: RAW data size (%zu) must be multiple of 5\n", raw_size);
        return -1;
    }
    
    size_t expected_pixels = width * height;
    size_t available_pixels = raw_size / 5 * 4;
    
    if (available_pixels < expected_pixels) {
        printf("Warning: Not enough RAW data (%zu pixels available, %zu expected)\n", 
               available_pixels, expected_pixels);
    }
    
    // 解包RAW10数据
    size_t raw_pos = 0;
    size_t pixel_pos = 0;
    size_t max_pixels = (available_pixels < expected_pixels) ? available_pixels : expected_pixels;
    
    while (raw_pos + 5 <= raw_size && pixel_pos + 4 <= max_pixels) {
        uint16_t pixels[4];
        unpack_sbggr10_scalar(raw_data + raw_pos, pixels);
        
        // 复制像素数据，注意边界检查
        for (int i = 0; i < 4 && pixel_pos < max_pixels; i++) {
            output_pixels[pixel_pos++] = pixels[i];
        }
        
        raw_pos += 5;
    }
    
    // 填充剩余像素（如果有）
    while (pixel_pos < expected_pixels) {
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
static void scale_pixels(const uint16_t* src_pixels, int src_width, int src_height,
                        uint16_t* dst_pixels, int dst_width, int dst_height) {
    float x_ratio = (float)src_width / dst_width;
    float y_ratio = (float)src_height / dst_height;
    
    for (int y = 0; y < dst_height; y++) {
        for (int x = 0; x < dst_width; x++) {
            int src_x = (int)(x * x_ratio);
            int src_y = (int)(y * y_ratio);
            
            // 边界检查
            if (src_x >= src_width) src_x = src_width - 1;
            if (src_y >= src_height) src_y = src_height - 1;
            
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
static void convert_pixels_to_rgb565(const uint16_t* pixels, uint16_t* rgb565_data,
                                    int width, int height) {
    int total_pixels = width * height;
    
    for (int i = 0; i < total_pixels; i++) {
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
 * @param dst_buffer 目标全屏缓冲区 (320x240)
 * @return 0 成功，-1 失败
 */
static int landscape_image_fit(const uint16_t* src_buffer, int src_width, int src_height, 
                       uint16_t* dst_buffer) {
    if (!src_buffer || !dst_buffer || src_width <= 0 || src_height <= 0) {
        return -1;
    }
    
    // 清空目标缓冲区 (黑色背景)
    memset(dst_buffer, 0, DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t));
    
    // 计算居中位置
    int x_offset = (DISPLAY_WIDTH - src_width) / 2;
    int y_offset = (DISPLAY_HEIGHT - src_height) / 2;
    
    // 确保不超出边界
    if (x_offset < 0) x_offset = 0;
    if (y_offset < 0) y_offset = 0;
    
    int copy_width = (src_width > DISPLAY_WIDTH) ? DISPLAY_WIDTH : src_width;
    int copy_height = (src_height > DISPLAY_HEIGHT) ? DISPLAY_HEIGHT : src_height;
    
    // 复制图像数据到居中位置
    for (int y = 0; y < copy_height; y++) {
        int dst_y = y + y_offset;
        if (dst_y >= DISPLAY_HEIGHT) break;
        
        for (int x = 0; x < copy_width; x++) {
            int dst_x = x + x_offset;
            if (dst_x >= DISPLAY_WIDTH) break;
            
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
static void update_image_display(void) {
    // 使用非阻塞锁尝试，避免阻塞按键处理
    if (pthread_mutex_trylock(&frame_mutex) != 0) {
        return; // 如果无法获取锁，跳过本次更新
    }
    
    if (frame_available && current_frame.data && img_canvas) {
        // 计算动态缩放尺寸
        int scaled_width, scaled_height;
        calculate_scaled_size(current_frame.width, current_frame.height, 
                             &scaled_width, &scaled_height);
        
        // 更新当前图像尺寸
        current_img_width = scaled_width;
        current_img_height = scaled_height;
        
        // 创建图像处理缓冲区 (静态分配，避免重复分配)
        static uint16_t unpacked_buffer[CAMERA_WIDTH * CAMERA_HEIGHT]; // 原始尺寸解包缓冲区
        static uint16_t scaled_pixels[DISPLAY_WIDTH * DISPLAY_HEIGHT];  // 缩放后的像素缓冲区
        static uint16_t scaled_rgb565[DISPLAY_WIDTH * DISPLAY_HEIGHT];  // RGB565缓冲区
        static uint16_t display_buffer[DISPLAY_WIDTH * DISPLAY_HEIGHT]; // 最终显示缓冲区
        static int last_processed_width = 0, last_processed_height = 0;
        
        // 只在尺寸变化时打印处理信息
        if (current_frame.width != last_processed_width || current_frame.height != last_processed_height) {
            printf("Processing frame: %dx%d -> %dx%d\n", 
                   current_frame.width, current_frame.height, scaled_width, scaled_height);
            last_processed_width = current_frame.width;
            last_processed_height = current_frame.height;
        }
        
        // 第一步：SBGGR10 解包到原始尺寸的16位像素数据
        if (unpack_sbggr10_image((const uint8_t*)current_frame.data, current_frame.size,
                                unpacked_buffer, current_frame.width, current_frame.height) != 0) {
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
        if (landscape_image_fit(scaled_rgb565, scaled_width, scaled_height, display_buffer) == 0) {
            // 创建 LVGL 图像描述符
            static lv_img_dsc_t img_dsc;
            img_dsc.header.always_zero = 0;
            img_dsc.header.w = DISPLAY_WIDTH;   // 使用全屏宽度
            img_dsc.header.h = DISPLAY_HEIGHT;  // 使用全屏高度
            img_dsc.data_size = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
            img_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
            img_dsc.data = (uint8_t*)display_buffer;
            
            // 更新图像 (使用全屏显示)
            lv_img_set_src(img_canvas, &img_dsc);
            lv_obj_set_size(img_canvas, DISPLAY_WIDTH, DISPLAY_HEIGHT);
            lv_obj_set_pos(img_canvas, 0, 0);  // 左上角对齐
            
            // 只在尺寸变化时打印成功信息
            if (current_frame.width != last_processed_width || current_frame.height != last_processed_height) {
                printf("Image updated: %dx%d -> %dx%d (SBGGR10 properly unpacked)\n", 
                       current_frame.width, current_frame.height, scaled_width, scaled_height);
            }
        } else {
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
 * @brief 摄像头采集线程函数
 */
static void* camera_thread(void* arg) {
    printf("Camera thread started\n");
    
    while (!exit_flag) {
        if (camera_paused) {
            usleep(100000); // 暂停时休眠100ms
            continue;
        }
        
        media_frame_t frame;
        
        // 采集帧数据 (减少超时时间，更快响应退出)
        int ret = libmedia_session_capture_frame(media_session, &frame, 50); // 50ms 超时
        if (ret == 0) {
            // 再次检查退出标志
            if (exit_flag) {
                libmedia_session_release_frame(media_session, &frame);
                break;
            }
            
            pthread_mutex_lock(&frame_mutex);
            
            // 更新当前帧
            if (current_frame.data) {
                libmedia_session_release_frame(media_session, &current_frame);
            }
            
            current_frame = frame;
            frame_available = 1;
            frame_count++;
            
            // 通知显示更新和TCP发送线程
            pthread_cond_broadcast(&frame_ready);
            pthread_mutex_unlock(&frame_mutex);
            
            // 更新帧率
            update_fps();
        } else if (ret != -EAGAIN) {
            if (!exit_flag) { // 只在未退出时打印错误
                printf("Failed to capture frame: %d\n", ret);
            }
            usleep(10000); // 10ms
        }
    }
    
    printf("Camera thread exited\n");
    return NULL;
}

// ============================================================================
// 按键处理函数
// ============================================================================

/**
 * @brief 处理按键输入 (优化响应速度，添加KEY1控制TCP传输，添加屏幕唤醒)
 */
static void handle_keys(void) {
    static int last_key0_state = 1;
    static int last_key1_state = 1;
    static int last_key2_state = 1;
    static int last_key3_state = 1;
    static int key0_debounce_count = 0;
    static int key1_debounce_count = 0;
    static int key2_debounce_count = 0;
    static int key3_debounce_count = 0;
    static struct timeval last_key_check = {0};
    const int debounce_threshold = 3; // 降低去抖动阈值，提高响应速度
    
    // 限制按键检查频率，但保持足够的响应速度
    struct timeval current_time;
    gettimeofday(&current_time, NULL);
    long time_since_last_check = (current_time.tv_sec - last_key_check.tv_sec) * 1000000 +
                                (current_time.tv_usec - last_key_check.tv_usec);
    
    // 每1ms检查一次按键状态
    if (time_since_last_check < 1000) {
        return;
    }
    last_key_check = current_time;
    
    // 读取按键状态
    int current_key0 = GET_KEY0;
    int current_key1 = GET_KEY1;
    int current_key2 = GET_KEY2;
    int current_key3 = GET_KEY3;
    
    // 检查任意按键是否被按下（屏幕唤醒）
    int any_key_pressed = (current_key0 == 0) || (current_key1 == 0) || 
                          (current_key2 == 0) || (current_key3 == 0);
    
    if (any_key_pressed && !screen_on) {
        turn_screen_on();
        return; // 屏幕唤醒后跳过其他按键处理，避免误操作
    }
    
    // 屏幕开启时才处理按键功能
    if (!screen_on) return;
    
    // KEY0 去抖动处理 (摄像头暂停/恢复)
    if (current_key0 == last_key0_state) {
        key0_debounce_count = 0;
    } else {
        key0_debounce_count++;
        if (key0_debounce_count >= debounce_threshold) {
            if (last_key0_state == 1 && current_key0 == 0) {
                // KEY0 按下事件 - 摄像头暂停/恢复
                camera_paused = !camera_paused;
                
                printf("Camera %s (Key response time optimized)\n", 
                       camera_paused ? "PAUSED" : "RESUMED");
                
                // 更新活动时间
                update_activity_time();
                
                // 更新状态显示
                if (status_label) {
                    lv_label_set_text(status_label, camera_paused ? "PAUSED" : "RUNNING");
                    lv_obj_set_style_text_color(status_label, 
                                               camera_paused ? lv_color_make(255, 0, 0) : lv_color_make(0, 255, 0), 0);
                }
                
                // 如果摄像头被暂停且TCP正在运行，关闭TCP
                if (camera_paused && tcp_enabled) {
                    printf("Camera paused, automatically disabling TCP transmission\n");
                    tcp_enabled = 0;
                    
                    // 关闭TCP连接
                    if (client_connected) {
                        shutdown(client_fd, SHUT_RDWR);
                        close(client_fd);
                        client_connected = 0;
                        client_fd = -1;
                    }
                    if (server_fd >= 0) {
                        shutdown(server_fd, SHUT_RDWR);
                        close(server_fd);
                        server_fd = -1;
                    }
                    
                    // 更新TCP状态显示
                    if (tcp_label) {
                        lv_label_set_text(tcp_label, "TCP: OFF");
                        lv_obj_set_style_text_color(tcp_label, lv_color_make(128, 128, 128), 0);
                    }
                }
            }
            last_key0_state = current_key0;
            key0_debounce_count = 0;
        }
    }
    
    // KEY1 去抖动处理 (TCP传输开关)
    if (current_key1 == last_key1_state) {
        key1_debounce_count = 0;
    } else {
        key1_debounce_count++;
        if (key1_debounce_count >= debounce_threshold) {
            if (last_key1_state == 1 && current_key1 == 0) {
                // KEY1 按下事件 - TCP传输开关
                
                // 检查摄像头是否暂停
                if (camera_paused) {
                    printf("Cannot enable TCP: Camera is paused. Resume camera first.\n");
                    
                    // 更新TCP状态显示为错误状态
                    if (tcp_label) {
                        lv_label_set_text(tcp_label, "TCP: ERR");
                        lv_obj_set_style_text_color(tcp_label, lv_color_make(255, 128, 0), 0);
                    }
                    
                    // 2秒后恢复正常显示
                    usleep(2000000);
                    if (tcp_label) {
                        lv_label_set_text(tcp_label, "TCP: OFF");
                        lv_obj_set_style_text_color(tcp_label, lv_color_make(128, 128, 128), 0);
                    }
                } else {
                    tcp_enabled = !tcp_enabled;
                    
                    printf("TCP transmission %s\n", tcp_enabled ? "ENABLED" : "DISABLED");
                    
                    // 更新活动时间
                    update_activity_time();
                    
                    if (tcp_enabled) {
                        // 启动TCP传输
                        if (server_fd < 0) {
                            server_fd = create_server(DEFAULT_PORT);
                            if (server_fd >= 0) {
                                if (pthread_create(&tcp_thread_id, NULL, tcp_sender_thread, NULL) == 0) {
                                    printf("TCP server started successfully\n");
                                    // 更新TCP状态显示
                                    if (tcp_label) {
                                        lv_label_set_text(tcp_label, "TCP: ON");
                                        lv_obj_set_style_text_color(tcp_label, lv_color_make(0, 255, 255), 0);
                                    }
                                } else {
                                    printf("Failed to create TCP thread\n");
                                    close(server_fd);
                                    server_fd = -1;
                                    tcp_enabled = 0;
                                }
                            } else {
                                printf("Failed to create TCP server\n");
                                tcp_enabled = 0;
                            }
                        }
                    } else {
                        // 停止TCP传输
                        printf("Stopping TCP transmission...\n");
                        
                        // 关闭客户端连接
                        if (client_connected) {
                            shutdown(client_fd, SHUT_RDWR);
                            close(client_fd);
                            client_connected = 0;
                            client_fd = -1;
                            printf("Client connection closed\n");
                        }
                        
                        // 关闭服务器socket
                        if (server_fd >= 0) {
                            shutdown(server_fd, SHUT_RDWR);
                            close(server_fd);
                            server_fd = -1;
                            printf("Server socket closed\n");
                        }
                        
                        // 等待TCP线程退出
                        pthread_cond_broadcast(&frame_ready); // 唤醒TCP线程
                        void* tcp_ret;
                        struct timespec timeout;
                        clock_gettime(CLOCK_REALTIME, &timeout);
                        timeout.tv_sec += 2; // 2秒超时
                        
                        int join_result = pthread_timedjoin_np(tcp_thread_id, &tcp_ret, &timeout);
                        if (join_result == 0) {
                            printf("TCP thread exited successfully\n");
                        } else if (join_result == ETIMEDOUT) {
                            printf("Warning: TCP thread timeout, forcing cancel\n");
                            pthread_cancel(tcp_thread_id);
                            pthread_join(tcp_thread_id, NULL);
                        } else {
                            printf("Warning: TCP thread join failed: %d\n", join_result);
                        }
                        
                        // 短暂延迟确保端口完全释放
                        usleep(100000); // 100ms
                        
                        // 更新TCP状态显示
                        if (tcp_label) {
                            lv_label_set_text(tcp_label, "TCP: OFF");
                            lv_obj_set_style_text_color(tcp_label, lv_color_make(128, 128, 128), 0);
                        }
                        printf("TCP transmission stopped completely\n");
                    }
                }
            }
            last_key1_state = current_key1;
            key1_debounce_count = 0;
        }
    }
    
    // KEY2 和 KEY3 去抖动处理（仅用于屏幕唤醒）
    if (current_key2 == last_key2_state) {
        key2_debounce_count = 0;
    } else {
        key2_debounce_count++;
        if (key2_debounce_count >= debounce_threshold) {
            if (last_key2_state == 1 && current_key2 == 0) {
                // KEY2 按下事件 - 仅更新活动时间
                update_activity_time();
                printf("KEY2 pressed (activity updated)\n");
            }
            last_key2_state = current_key2;
            key2_debounce_count = 0;
        }
    }
    
    if (current_key3 == last_key3_state) {
        key3_debounce_count = 0;
    } else {
        key3_debounce_count++;
        if (key3_debounce_count >= debounce_threshold) {
            if (last_key3_state == 1 && current_key3 == 0) {
                // KEY3 按下事件 - 仅更新活动时间
                update_activity_time();
                printf("KEY3 pressed (activity updated)\n");
            }
            last_key3_state = current_key3;
            key3_debounce_count = 0;
        }
    }
}

// ============================================================================
// LVGL 界面初始化
// ============================================================================

/**
 * @brief 初始化 LVGL 界面
 */
static void init_lvgl_ui(void) {
    // 获取当前屏幕
    lv_obj_t* scr = lv_disp_get_scr_act(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    
    // 创建图像显示区域 (使用全屏尺寸320x240)
    img_canvas = lv_img_create(scr);
    lv_obj_set_pos(img_canvas, 0, 0);
    lv_obj_set_size(img_canvas, 320, 240);  // 强制使用全屏尺寸
    
    // 创建系统信息标签 (左上角，合并显示图像大小、帧率、CPU和内存占用)
    info_label = lv_label_create(scr);
    lv_label_set_text(info_label, "0x0 0.0 0% 0%");
    lv_obj_set_style_text_color(info_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(info_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_bg_color(info_label, lv_color_make(0, 0, 0), 0);
    lv_obj_set_style_bg_opa(info_label, LV_OPA_50, 0);
    lv_obj_set_style_pad_all(info_label, 2, 0);
    lv_obj_align(info_label, LV_ALIGN_TOP_LEFT, 5, 5);
    
    // 创建状态标签 (右下角，替代按键提示)
    status_label = lv_label_create(scr);
    lv_label_set_text(status_label, "RUNNING");
    lv_obj_set_style_text_color(status_label, lv_color_make(0, 255, 0), 0);
    lv_obj_set_style_text_font(status_label, &lv_font_montserrat_14, 0);
    // 添加半透明背景以提高可读性
    lv_obj_set_style_bg_color(status_label, lv_color_make(0, 0, 0), 0);
    lv_obj_set_style_bg_opa(status_label, LV_OPA_50, 0);
    lv_obj_set_style_pad_all(status_label, 2, 0);
    lv_obj_align(status_label, LV_ALIGN_BOTTOM_RIGHT, -5, -5);
    
    // 创建TCP状态标签 (左下角，显示TCP传输状态)
    tcp_label = lv_label_create(scr);
    lv_label_set_text(tcp_label, "TCP: OFF");
    lv_obj_set_style_text_color(tcp_label, lv_color_make(128, 128, 128), 0);
    lv_obj_set_style_text_font(tcp_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_bg_color(tcp_label, lv_color_make(0, 0, 0), 0);
    lv_obj_set_style_bg_opa(tcp_label, LV_OPA_50, 0);
    lv_obj_set_style_pad_all(tcp_label, 2, 0);
    lv_obj_align(tcp_label, LV_ALIGN_BOTTOM_LEFT, 5, -5);
    
    printf("LVGL UI initialized (landscape mode: %dx%d)\n", DISPLAY_WIDTH, DISPLAY_HEIGHT);
}

// ============================================================================
// 主函数
// ============================================================================

int main(void) {
    printf("LVGL Camera Display System Starting...\n");
    
    // 设置信号处理
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    // 初始化 LVGL
    lv_init();
    
    // 检查显示配置
    check_display_config();
    
    // 初始化帧缓冲设备
    fbdev_init();
    
    // 检查帧缓冲设备信息并尝试设置横屏
    printf("Checking framebuffer configuration...\n");
    system("fbset | grep geometry");
    
    // 尝试设置帧缓冲为横屏模式
    // 注意：这可能需要root权限和设备支持
    printf("Attempting to set landscape mode...\n");
    int fb_ret = system("fbset -xres 320 -yres 240 2>/dev/null");
    if (fb_ret == 0) {
        printf("Framebuffer set to 320x240\n");
    } else {
        printf("Warning: Could not set framebuffer resolution\n");
    }
    
    // 创建 LVGL 显示缓冲区
    static lv_color_t buf[DISP_BUF_SIZE];
    static lv_disp_draw_buf_t disp_buf;
    lv_disp_draw_buf_init(&disp_buf, buf, NULL, DISP_BUF_SIZE);
    
    // 注册显示驱动 (强制横屏模式: 320x240)
    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.draw_buf = &disp_buf;
    disp_drv.flush_cb = fbdev_flush;
    disp_drv.hor_res = 320;  // 强制设置横屏宽度
    disp_drv.ver_res = 240;  // 强制设置横屏高度
    
    // 尝试设置旋转（如果支持）
    // disp_drv.rotated = LV_DISP_ROT_90;  // 如果需要旋转90度
    
    lv_disp_drv_register(&disp_drv);
    
    // 初始化 GPIO
    if (DEV_ModuleInit() != 0) {
        printf("Failed to initialize GPIO\n");
        return -1;
    }
    
    // 初始化 libMedia
    if (libmedia_init() != 0) {
        printf("Failed to initialize libMedia\n");
        goto cleanup;
    }
    
    // 配置摄像头会话
    media_session_config_t config = {
        .device_path = CAMERA_DEVICE,
        .format = {
            .width = CAMERA_WIDTH,
            .height = CAMERA_HEIGHT,
            .pixelformat = CAMERA_PIXELFORMAT,
            .num_planes = 1,
            .plane_size = {CAMERA_WIDTH * CAMERA_HEIGHT * 2} // RAW10 约2字节/像素
        },
        .buffer_count = BUFFER_COUNT,
        .use_multiplanar = 1,
        .nonblocking = 0
    };
    
    // 创建媒体会话
    media_session = libmedia_create_session(&config);
    if (!media_session) {
        printf("Failed to create media session: %s\n", 
               libmedia_get_error_string(libmedia_get_last_error()));
        goto cleanup;
    }
    
    // 启动摄像头流
    if (libmedia_start_session(media_session) < 0) {
        printf("Failed to start media session: %s\n", 
               libmedia_get_error_string(libmedia_get_last_error()));
        goto cleanup;
    }
    
    printf("Camera session started successfully\n");
    
    // 初始化 LVGL 界面
    init_lvgl_ui();
    
    // 初始化帧率统计
    gettimeofday(&last_fps_time, NULL);
    
    // 初始化屏幕活动时间
    update_activity_time();
    
    // 启动摄像头采集线程
    pthread_t camera_tid;
    if (pthread_create(&camera_tid, NULL, camera_thread, NULL) != 0) {
        printf("Failed to create camera thread\n");
        goto cleanup;
    }
    
    printf("System initialized successfully\n");
    printf("Display: 320x240 (forced landscape mode)\n");
    printf("Camera: %dx%d (RAW10)\n", CAMERA_WIDTH, CAMERA_HEIGHT);
    printf("Scaling: Width-aligned to 320px, maintaining aspect ratio\n");
    printf("Performance optimizations enabled:\n");
    printf("  - Display update rate limited to 30 FPS\n");
    printf("  - Non-blocking frame mutex for better key response\n");
    printf("  - Optimized key debouncing (3 samples)\n");
    printf("  - Reduced debug output for better performance\n");
    printf("Controls:\n");
    printf("  KEY0 (PIN %d) - Pause/Resume camera\n", KEY0_PIN);
    printf("  KEY1 (PIN %d) - Enable/Disable TCP transmission\n", KEY1_PIN);
    printf("  KEY2 (PIN %d) - Wake screen / Update activity\n", KEY2_PIN);
    printf("  KEY3 (PIN %d) - Wake screen / Update activity\n", KEY3_PIN);
    printf("  Any key - Wake screen if auto-sleep activated\n");
    printf("  Ctrl+C - Exit\n");
    printf("Screen Management:\n");
    printf("  - Auto-sleep after 5s camera pause\n");
    printf("  - Wake with any key press\n");
    printf("TCP Restrictions:\n");
    printf("  - TCP can only be enabled when camera is running\n");
    printf("  - TCP auto-disabled when camera is paused\n");
    printf("TCP Server: %s:%d (disabled by default)\n", DEFAULT_SERVER_IP, DEFAULT_PORT);
    
    // 主循环
    uint32_t loop_count = 0;
    struct timeval last_display_update = {0};
    gettimeofday(&last_display_update, NULL);
    
    while (!exit_flag) {
        struct timeval current_time;
        gettimeofday(&current_time, NULL);
        
        // 处理 LVGL 任务 (高优先级，每次循环都执行)
        lv_timer_handler();
        
        // 再次检查退出标志
        if (exit_flag) break;
        
        // 处理按键 (高优先级，每次循环都执行)
        handle_keys();
        
        // 再次检查退出标志
        if (exit_flag) break;
        
        // 检查屏幕自动关闭（摄像头暂停5秒后）
        check_screen_timeout();
        
        // 限制图像显示更新频率到30FPS (33ms间隔)
        long display_time_diff = (current_time.tv_sec - last_display_update.tv_sec) * 1000000 +
                                (current_time.tv_usec - last_display_update.tv_usec);
        
        if (display_time_diff >= 33333) { // 30 FPS = 33.33ms
            // 只在屏幕开启时更新图像显示
            if (screen_on) {
                update_image_display();
            }
            last_display_update = current_time;
        }
        
        // 更新系统信息 (CPU和内存占用) - 只在屏幕开启时更新
        if (screen_on) {
            update_system_info();
        }
        
        // 动态休眠时间：降低CPU占用
        loop_count++;
        if (loop_count % 10 == 0) {
            usleep(10000); // 每10次循环休眠10ms
        } else {
            usleep(1000);  // 其他时候休眠1ms
        }
    }
    
    printf("Main loop exited, shutting down...\n");
    
    // 停止TCP传输
    tcp_enabled = 0;
    if (client_connected && client_fd >= 0) {
        close(client_fd);
        client_connected = 0;
    }
    if (server_fd >= 0) {
        close(server_fd);
        server_fd = -1;
    }
    
    // 确保停止媒体会话，避免阻塞
    printf("Stopping media session...\n");
    if (media_session) {
        libmedia_stop_session(media_session);
    }
    
    // 等待摄像头线程结束 (设置超时)
    printf("Waiting for camera thread to exit...\n");
    struct timespec timeout;
    clock_gettime(CLOCK_REALTIME, &timeout);
    timeout.tv_sec += 3; // 3秒超时
    
    int join_result = pthread_timedjoin_np(camera_tid, NULL, &timeout);
    if (join_result == ETIMEDOUT) {
        printf("Warning: Camera thread did not exit within timeout, canceling...\n");
        pthread_cancel(camera_tid);
        pthread_join(camera_tid, NULL);
    } else if (join_result != 0) {
        printf("Warning: pthread_join failed: %d\n", join_result);
    } else {
        printf("Camera thread joined successfully\n");
    }
    
cleanup:
    // 等待TCP线程结束
    if (tcp_enabled) {
        printf("Waiting for TCP thread to exit...\n");
        tcp_enabled = 0;
        pthread_cond_broadcast(&frame_ready);
        void* tcp_ret;
        if (pthread_join(tcp_thread_id, &tcp_ret) == 0) {
            printf("TCP thread exited successfully\n");
        }
    }
    
    // 清理TCP资源
    if (client_connected && client_fd >= 0) {
        close(client_fd);
        client_connected = 0;
    }
    if (server_fd >= 0) {
        close(server_fd);
        server_fd = -1;
    }
    
    // 清理当前帧数据
    printf("Cleaning up frame data...\n");
    pthread_mutex_lock(&frame_mutex);
    if (current_frame.data) {
        libmedia_session_release_frame(media_session, &current_frame);
        current_frame.data = NULL;
    }
    pthread_mutex_unlock(&frame_mutex);
    
    // 清理媒体会话
    printf("Cleaning up media session...\n");
    if (media_session) {
        libmedia_stop_session(media_session);
        libmedia_destroy_session(media_session);
        media_session = NULL;
    }
    
    // 清理 libMedia
    printf("Deinitializing libMedia...\n");
    libmedia_deinit();
    
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
