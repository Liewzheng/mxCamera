/*****************************************************************************
* | File      	:   DEV_Config.h
* | Author      :   Luckfox team
* | Function    :   Hardware underlying interface
* | Info        :
*----------------
* |	This version:   V2.0
* | Date        :   2019-07-08
* | Info        :   Basic version
*
******************************************************************************/
#ifndef _DEV_CONFIG_H_
#define _DEV_CONFIG_H_

#include "Debug.h"

#include <gpio.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

/**
 * data
**/
#define UBYTE   uint8_t
#define UWORD   uint16_t
#define UDOUBLE uint32_t

// ========================== 硬件按键引脚定义 ==========================
// 根据硬件连接表，四个独立按键映射：
// LCD PIN    PICO PIN     Pin number
// KEY0   →   GPIO1_D1_D → 57
// KEY1   →   GPIO4_B0_D → 136  
// KEY2   →   GPIO1_C7_D → 55
// KEY3   →   GPIO1_C6_D → 54

#define KEY0_PIN        57      // KEY0 - GPIO1_D1_D (主要动作键)
#define KEY1_PIN        136     // KEY1 - GPIO4_B0_D (功能键1)
#define KEY2_PIN        55      // KEY2 - GPIO1_C7_D (功能键2) 
#define KEY3_PIN        54      // KEY3 - GPIO1_C6_D (功能键3)

// 按键功能映射 (根据应用需求可重新定义)
#define KEY_UP_PIN      KEY0_PIN    // 方向键：上
#define KEY_DOWN_PIN    KEY1_PIN    // 方向键：下  
#define KEY_LEFT_PIN    KEY2_PIN    // 方向键：左
#define KEY_RIGHT_PIN   KEY3_PIN    // 方向键：右

// LCD 相关引脚定义 (基于 fbtft 标准配置)
#define LCD_CS_PIN      48     // SPI 片选信号
#define LCD_RST_PIN     51     // LCD 复位信号  
#define LCD_DC_PIN      34     // 数据/命令选择信号
#define LCD_BL_PIN      4      // 背光控制信号

// SPI 总线引脚 (通常由系统管理，此处仅作参考)
#define LCD_CLK_PIN     49     // SPI 时钟
#define LCD_MOSI_PIN    50     // SPI 数据输出
#define LCD_MISO_PIN    9      // SPI 数据输入 (可选)

// LCD 规格定义 (横屏模式)
#define LCD_WIDTH       320    // LCD 宽度 (横屏)
#define LCD_HEIGHT      240    // LCD 高度 (横屏)

// LCD 控制宏定义
#define LCD_CS(x)       DEV_Digital_Write(LCD_CS_PIN, x)
#define LCD_RST(x)      DEV_Digital_Write(LCD_RST_PIN, x)
#define LCD_DC(x)       DEV_Digital_Write(LCD_DC_PIN, x)
#define LCD_BL(x)       DEV_Digital_Write(LCD_BL_PIN, x)


// ========================== 按键读取宏定义 ==========================
// 硬件按键直接读取 (低电平有效，按下为0，松开为1)
#define GET_KEY0                DEV_Digital_Read(KEY0_PIN)
#define GET_KEY1                DEV_Digital_Read(KEY1_PIN)
#define GET_KEY2                DEV_Digital_Read(KEY2_PIN)
#define GET_KEY3                DEV_Digital_Read(KEY3_PIN)

// 方向键功能映射
#define GET_KEY_UP              DEV_Digital_Read(KEY_UP_PIN)
#define GET_KEY_DOWN            DEV_Digital_Read(KEY_DOWN_PIN)
#define GET_KEY_LEFT            DEV_Digital_Read(KEY_LEFT_PIN)
#define GET_KEY_RIGHT           DEV_Digital_Read(KEY_RIGHT_PIN)

/*------------------------------------------------------------------------------------------------------*/
UBYTE DEV_ModuleInit(void);
void DEV_ModuleExit(void);

void DEV_GPIO_Mode(UWORD Pin, UWORD Mode);
void DEV_GPIO_Mode_PullUp(UWORD Pin);
void DEV_Digital_Write(UWORD Pin, UBYTE Value);
UBYTE DEV_Digital_Read(UWORD Pin);
void DEV_Delay_ms(UDOUBLE xms);
#endif
