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

#define KEY0_PIN	 (132)	// GPIO4_A4_D
#define KEY1_PIN	 (131)	// GPIO4_A3_D
#define KEY2_PIN	 (130)	// GPIO4_A2_D
#define KEY3_PIN	 (134)	// GPIO4_A6_D
#define KEYX_PIN	 (55)	// GPIO1_C7_D
#define POWER_PIN    (54)	// GPIO1_C6_D

// 按键功能映射 (根据应用需求可重新定义)
#define KEY_UP_PIN      KEY0_PIN    // 方向键：上
#define KEY_DOWN_PIN    KEY1_PIN    // 方向键：下  
#define KEY_LEFT_PIN    KEY2_PIN    // 方向键：左
#define KEY_RIGHT_PIN   KEY3_PIN    // 方向键：右
#define KEY_X_PIN       KEYX_PIN    
#define KEY_POWER_PIN   POWER_PIN // 电源键

// LCD 规格定义 (横屏模式)
#define LCD_WIDTH       240    // LCD 宽度 (横屏)
#define LCD_HEIGHT      240    // LCD 高度 (横屏)

// ========================== 按键读取宏定义 ==========================
// 硬件按键直接读取 (低电平有效，按下为0，松开为1)
#define GET_KEY0                DEV_Digital_Read(KEY0_PIN)
#define GET_KEY1                DEV_Digital_Read(KEY1_PIN)
#define GET_KEY2                DEV_Digital_Read(KEY2_PIN)
#define GET_KEY3                DEV_Digital_Read(KEY3_PIN)
#define GET_KEYX                DEV_Digital_Read(KEYX_PIN)

/*------------------------------------------------------------------------------------------------------*/
UBYTE DEV_ModuleInit(void);
void DEV_ModuleExit(void);

void DEV_GPIO_Mode(UWORD Pin, UWORD Mode);
void DEV_GPIO_Mode_PullUp(UWORD Pin);
void DEV_Digital_Write(UWORD Pin, UBYTE Value);
UBYTE DEV_Digital_Read(UWORD Pin);
void DEV_Delay_ms(UDOUBLE xms);
#endif
