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
 */

#include <pthread.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

// 库头文件
#include <gpio.h>
#include <media.h>
#include "DEV_Config.h"
#include "lv_drivers/display/fbdev.h"
#include "lvgl/lvgl.h"

// ============================================================================
// 系统配置常量
// ============================================================================

#define DISP_BUF_SIZE (240 * 320)

// 摄像头配置
#define CAMERA_WIDTH 1920
#define CAMERA_HEIGHT 1080
#define CAMERA_PIXELFORMAT V4L2_PIX_FMT_SBGGR10
#define CAMERA_DEVICE "/dev/video0"
#define BUFFER_COUNT 4

// 显示配置
#define DISPLAY_WIDTH 240
#define DISPLAY_HEIGHT 320

// 帧率统计配置
#define FPS_UPDATE_INTERVAL 1000000 // 1秒 (微秒)

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
void signal_handler(int sig) {
    printf("\nReceived signal %d, cleaning up...\n", sig);
    exit_flag = 1;
    
    // 通知所有等待线程
    pthread_mutex_lock(&frame_mutex);
    pthread_cond_broadcast(&frame_ready);
    pthread_mutex_unlock(&frame_mutex);
}

/**
 * @brief 计算帧率
 */
void update_fps() {
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
            lv_label_set_text_fmt(fps_label, "FPS: %.1f", current_fps);
        }
    }
}

/**
 * @brief RAW10 到 RGB565 转换 (简化版本)
 * @param raw_data RAW10 数据
 * @param rgb_data 输出 RGB565 数据
 * @param width 图像宽度
 * @param height 图像高度
 */
void convert_raw10_to_rgb565(const uint8_t* raw_data, uint16_t* rgb_data, 
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
 * @brief 更新图像显示
 */
void update_image_display() {
    pthread_mutex_lock(&frame_mutex);
    
    if (frame_available && current_frame.data && img_canvas) {
        // 创建 RGB565 缓冲区
        static uint16_t rgb_buffer[DISPLAY_WIDTH * DISPLAY_HEIGHT];
        
        // 转换 RAW10 到 RGB565
        convert_raw10_to_rgb565((const uint8_t*)current_frame.data, rgb_buffer,
                               current_frame.width, current_frame.height,
                               DISPLAY_WIDTH, DISPLAY_HEIGHT);
        
        // 创建 LVGL 图像描述符
        static lv_img_dsc_t img_dsc = {
            .header.always_zero = 0,
            .header.w = DISPLAY_WIDTH,
            .header.h = DISPLAY_HEIGHT,
            .data_size = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t),
            .header.cf = LV_IMG_CF_TRUE_COLOR,
            .data = (uint8_t*)rgb_buffer
        };
        
        // 更新图像
        lv_img_set_src(img_canvas, &img_dsc);
        
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
void* camera_thread(void* arg) {
    printf("Camera thread started\n");
    
    while (!exit_flag) {
        if (camera_paused) {
            usleep(100000); // 暂停时休眠100ms
            continue;
        }
        
        media_frame_t frame;
        
        // 采集帧数据
        int ret = libmedia_session_capture_frame(media_session, &frame, 100);
        if (ret == 0) {
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
            printf("Failed to capture frame: %d\n", ret);
            usleep(10000);
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
void handle_keys() {
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
void init_lvgl_ui() {
    // 获取当前屏幕
    lv_obj_t* scr = lv_disp_get_scr_act(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    
    // 创建图像显示区域
    img_canvas = lv_img_create(scr);
    lv_obj_set_pos(img_canvas, 0, 0);
    lv_obj_set_size(img_canvas, DISPLAY_WIDTH, DISPLAY_HEIGHT);
    
    // 创建 FPS 标签 (右上角)
    fps_label = lv_label_create(scr);
    lv_label_set_text(fps_label, "FPS: 0.0");
    lv_obj_set_style_text_color(fps_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(fps_label, &lv_font_montserrat_14, 0);
    lv_obj_align(fps_label, LV_ALIGN_TOP_RIGHT, -5, 5);
    
    // 创建状态标签 (左上角)
    status_label = lv_label_create(scr);
    lv_label_set_text(status_label, "RUNNING");
    lv_obj_set_style_text_color(status_label, lv_color_make(0, 255, 0), 0);
    lv_obj_set_style_text_font(status_label, &lv_font_montserrat_14, 0);
    lv_obj_align(status_label, LV_ALIGN_TOP_LEFT, 5, 5);
    
    printf("LVGL UI initialized\n");
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
    
    // 初始化帧缓冲设备
    fbdev_init();
    
    // 创建 LVGL 显示缓冲区
    static lv_color_t buf[DISP_BUF_SIZE];
    static lv_disp_draw_buf_t disp_buf;
    lv_disp_draw_buf_init(&disp_buf, buf, NULL, DISP_BUF_SIZE);
    
    // 注册显示驱动
    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.draw_buf = &disp_buf;
    disp_drv.flush_cb = fbdev_flush;
    disp_drv.hor_res = LCD_WIDTH;
    disp_drv.ver_res = LCD_HEIGHT;
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
    printf("Controls:\n");
    printf("  KEY0 (PIN %d) - Pause/Resume camera\n", KEY0_PIN);
    printf("  Ctrl+C - Exit\n");
    
    // 主循环
    while (!exit_flag) {
        // 处理 LVGL 任务
        lv_timer_handler();
        
        // 处理按键
        handle_keys();
        
        // 更新图像显示
        update_image_display();
        
        // 短暂休眠
        usleep(5000); // 5ms
    }
    
    printf("Shutting down...\n");
    
    // 等待摄像头线程结束
    pthread_join(camera_tid, NULL);
    
cleanup:
    // 清理媒体会话
    if (media_session) {
        libmedia_stop_session(media_session);
        libmedia_destroy_session(media_session);
    }
    
    // 清理 libMedia
    libmedia_deinit();
    
    // 清理 GPIO
    DEV_ModuleExit();
    
    printf("System shutdown complete\n");
    return 0;
}
