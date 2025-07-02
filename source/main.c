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
static void convert_raw10_to_rgb565(const uint8_t* raw_data, uint16_t* rgb_data, 
                                   int src_width, int src_height,
                                   int dst_width, int dst_height);
static int landscape_image_fit(const uint16_t* src_buffer, int src_width, int src_height, 
                              uint16_t* dst_buffer);

// 显示和UI更新
static void update_fps(void);
static void update_image_display(void);
static void init_lvgl_ui(void);

// 线程和输入处理
static void* camera_thread(void* arg);
static void handle_keys(void);

// ============================================================================
// 全局变量
// ============================================================================

// 系统状态
static volatile int exit_flag = 0;
static volatile int camera_paused = 0;

// 媒体会话
static media_session_t* media_session = NULL;

// LVGL 对象
static lv_obj_t* img_canvas = NULL;
static lv_obj_t* fps_label = NULL;
static lv_obj_t* status_label = NULL;
static lv_obj_t* info_label = NULL;

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
    
    // 强制刷新输出缓冲区
    fflush(stdout);
    fflush(stderr);
    
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
        
        // 更新 FPS 显示
        if (fps_label) {
            char fps_text[32];
            snprintf(fps_text, sizeof(fps_text), "FPS: %.1f", (double)current_fps);
            lv_label_set_text(fps_label, fps_text);
        }
    }
}

/**
 * @brief RAW10 到 RGB565 转换 (简化版本)
 * @param raw_data RAW10 数据
 * @param rgb_data 输出 RGB565 数据
 * @param src_width 源图像宽度
 * @param src_height 源图像高度
 * @param dst_width 目标图像宽度
 * @param dst_height 目标图像高度
 */
static void convert_raw10_to_rgb565(const uint8_t* raw_data, uint16_t* rgb_data, 
                            int src_width, int src_height,
                            int dst_width, int dst_height) {
    // 简化的 RAW10 转换：只取高8位，当作灰度
    // 实际应用中需要完整的 demosaic 算法
    
    float x_ratio = (float)src_width / dst_width;
    float y_ratio = (float)src_height / dst_height;
    
    for (int y = 0; y < dst_height; y++) {
        for (int x = 0; x < dst_width; x++) {
            int src_x = (int)(x * x_ratio);
            int src_y = (int)(y * y_ratio);
            
            // RAW10 格式：每5个字节包含4个像素
            int pixel_idx = src_y * src_width + src_x;
            int byte_idx = (pixel_idx / 4) * 5;
            int pixel_in_group = pixel_idx % 4;
            
            uint16_t raw_value;
            
            if (byte_idx + 4 < src_width * src_height * 5 / 4) {
                switch (pixel_in_group) {
                    case 0:
                        raw_value = (raw_data[byte_idx] << 2) | ((raw_data[byte_idx + 4] >> 0) & 0x03);
                        break;
                    case 1:
                        raw_value = (raw_data[byte_idx + 1] << 2) | ((raw_data[byte_idx + 4] >> 2) & 0x03);
                        break;
                    case 2:
                        raw_value = (raw_data[byte_idx + 2] << 2) | ((raw_data[byte_idx + 4] >> 4) & 0x03);
                        break;
                    case 3:
                        raw_value = (raw_data[byte_idx + 3] << 2) | ((raw_data[byte_idx + 4] >> 6) & 0x03);
                        break;
                    default:
                        raw_value = 0;
                }
            } else {
                raw_value = 0;
            }
            
            // 转换为8位灰度值
            uint8_t gray = (uint8_t)(raw_value >> 2);
            
            // 转换为 RGB565 (灰度图)
            uint16_t rgb565 = ((gray >> 3) << 11) | ((gray >> 2) << 5) | (gray >> 3);
            
            rgb_data[y * dst_width + x] = rgb565;
        }
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
 * @brief 更新图像显示 (使用高效横屏适配)
 */
static void update_image_display(void) {
    pthread_mutex_lock(&frame_mutex);
    
    if (frame_available && current_frame.data && img_canvas) {
        // 计算动态缩放尺寸
        int scaled_width, scaled_height;
        calculate_scaled_size(current_frame.width, current_frame.height, 
                             &scaled_width, &scaled_height);
        
        // 更新当前图像尺寸
        current_img_width = scaled_width;
        current_img_height = scaled_height;
        
        // 创建图像处理缓冲区
        static uint16_t scaled_buffer[DISPLAY_WIDTH * DISPLAY_HEIGHT];
        static uint16_t display_buffer[DISPLAY_WIDTH * DISPLAY_HEIGHT];
        
        // 第一步：RAW10 转换到 RGB565 (缩放到目标尺寸)
        convert_raw10_to_rgb565((const uint8_t*)current_frame.data, scaled_buffer,
                               current_frame.width, current_frame.height,
                               scaled_width, scaled_height);
        
        // 第二步：横屏适配 (居中显示到全屏缓冲区)
        if (landscape_image_fit(scaled_buffer, scaled_width, scaled_height, display_buffer) == 0) {
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
            
            // 更新分辨率信息显示
            if (info_label) {
                char info_text[64];
                snprintf(info_text, sizeof(info_text), "Cam: %dx%d", 
                        current_frame.width, current_frame.height);
                lv_label_set_text(info_label, info_text);
            }
            
            printf("Image updated: %dx%d -> %dx%d (landscape fit)\n", 
                   current_frame.width, current_frame.height, scaled_width, scaled_height);
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
            
            pthread_cond_signal(&frame_ready);
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
 * @brief 处理按键输入
 */
static void handle_keys(void) {
    static int last_key0_state = 1;
    static int key0_debounce_count = 0;
    const int debounce_threshold = 5;
    
    // 读取 KEY0 状态
    int current_key0 = GET_KEY0;
    
    // 去抖动处理
    if (current_key0 == last_key0_state) {
        key0_debounce_count = 0;
    } else {
        key0_debounce_count++;
        if (key0_debounce_count >= debounce_threshold) {
            if (last_key0_state == 1 && current_key0 == 0) {
                // 按键按下事件
                camera_paused = !camera_paused;
                
                printf("Camera %s\n", camera_paused ? "PAUSED" : "RESUMED");
                
                // 更新状态显示
                if (status_label) {
                    lv_label_set_text(status_label, camera_paused ? "PAUSED" : "RUNNING");
                    lv_obj_set_style_text_color(status_label, 
                                               camera_paused ? lv_color_make(255, 0, 0) : lv_color_make(0, 255, 0), 0);
                }
            }
            last_key0_state = current_key0;
            key0_debounce_count = 0;
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
    
    // 创建 FPS 标签 (右上角，横屏适配)
    fps_label = lv_label_create(scr);
    lv_label_set_text(fps_label, "FPS: 0.0");
    lv_obj_set_style_text_color(fps_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(fps_label, &lv_font_montserrat_14, 0);
    // 添加半透明背景以提高可读性
    lv_obj_set_style_bg_color(fps_label, lv_color_make(0, 0, 0), 0);
    lv_obj_set_style_bg_opa(fps_label, LV_OPA_50, 0);
    lv_obj_set_style_pad_all(fps_label, 2, 0);
    lv_obj_align(fps_label, LV_ALIGN_TOP_RIGHT, -5, 5);
    
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
    
    // 创建图像信息标签 (左下角，显示分辨率信息)
    info_label = lv_label_create(scr);
    lv_label_set_text(info_label, "Cam: 0x0");
    lv_obj_set_style_text_color(info_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(info_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_bg_color(info_label, lv_color_make(0, 0, 0), 0);
    lv_obj_set_style_bg_opa(info_label, LV_OPA_50, 0);
    lv_obj_set_style_pad_all(info_label, 2, 0);
    lv_obj_align(info_label, LV_ALIGN_BOTTOM_LEFT, 5, -5);
    
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
    printf("Example scaling:\n");
    printf("  1920x1080 -> 320x180 (16:9)\n");
    printf("  1600x1200 -> 320x240 (4:3)\n");
    printf("  1280x720  -> 320x180 (16:9)\n");
    printf("Controls:\n");
    printf("  KEY0 (PIN %d) - Pause/Resume camera\n", KEY0_PIN);
    printf("  Ctrl+C - Exit\n");
    
    // 主循环
    while (!exit_flag) {
        // 处理 LVGL 任务
        lv_timer_handler();
        
        // 再次检查退出标志
        if (exit_flag) break;
        
        // 处理按键
        handle_keys();
        
        // 再次检查退出标志
        if (exit_flag) break;
        
        // 更新图像显示
        update_image_display();
        
        // 短暂休眠
        usleep(5000); // 5ms
    }
    
    printf("Main loop exited, shutting down...\n");
    
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
