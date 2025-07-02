/*****************************************************************************
* | File      	:   DEV_Config.c
* | Author      :   Luckfox team
* | Function    :   Hardware underlying interface
* | Info        :
*----------------
* |	This version:   V2.0
* | Date        :   2019-07-08
* | Info        :   Basic version
*
******************************************************************************/
#include "DEV_Config.h"
#include <gpio.h>


/*****************************************
                GPIO
*****************************************/
void DEV_Digital_Write(UWORD Pin, UBYTE Value)
{
    int result = libgpio_write(Pin, Value);
    if (result < 0) {
        printf("Warning: Failed to write to GPIO %d\n", Pin);
    }
}

UBYTE DEV_Digital_Read(UWORD Pin)
{
    int result = libgpio_read(Pin);
    if (result < 0) {
        printf("Warning: Failed to read from GPIO %d\n", Pin);
        return 1; // 默认返回高电平（按键未按下状态）
    }
    return (UBYTE)result;
}

void DEV_GPIO_Mode(UWORD Pin, UWORD Mode)
{
    // 尝试导出 GPIO（即使失败也继续，可能已经被其他地方配置）
    libgpio_export(Pin);
    
    if(Mode == 0 || Mode == GPIO_IN){
        if (libgpio_set_direction(Pin, GPIO_IN) == GPIO_SUCCESS) {
            printf("IN Pin = %d\r\n",Pin);
        } else {
            printf("Warning: Failed to set Pin %d direction to input\r\n", Pin);
        }
    }else{
        if (libgpio_set_direction(Pin, GPIO_OUT) == GPIO_SUCCESS) {
            printf("OUT Pin = %d\r\n",Pin);
        } else {
            printf("Warning: Failed to set Pin %d direction to output\r\n", Pin);
        }
    }
}

/**
 * Configure GPIO input with pull-up resistor
 */
void DEV_GPIO_Mode_PullUp(UWORD Pin)
{
    // 尝试导出 GPIO（即使失败也继续）
    libgpio_export(Pin);
    
    if (libgpio_set_direction(Pin, GPIO_IN) == GPIO_SUCCESS) {
        libgpio_set_pull(Pin, GPIO_PULL_UP); // 忽略上拉配置的失败，因为有些平台不支持
        printf("IN Pin = %d (with pull-up)\r\n", Pin);
    } else {
        printf("Warning: Failed to configure Pin %d as input with pull-up\r\n", Pin);
    }
}

/**
 * delay x ms
**/
void DEV_Delay_ms(UDOUBLE xms)
{
    UDOUBLE i;
    for(i=0; i < xms; i++){
        usleep(1000);
    }
}

static void DEV_GPIO_Init(void)
{
    // 初始化 libgpio 库
    libgpio_init();
    
    // 按键引脚初始化 (输入模式，带上拉电阻)
    printf("Initializing GPIO pins...\n");

#if defined KEY_UP_PIN
    DEV_GPIO_Mode(KEY_UP_PIN, 1);
    DEV_Digital_Write(KEY_UP_PIN, 1); // 设置为高电平（上拉）
    printf("KEY_UP_PIN (%d) initialized as input with pull-up\n", KEY_UP_PIN);
#endif

#if defined KEY_DOWN_PIN
    DEV_GPIO_Mode(KEY_DOWN_PIN, 1);
    DEV_Digital_Write(KEY_DOWN_PIN, 1); // 设置为高电平（上拉）
    printf("KEY_DOWN_PIN (%d) initialized as input with pull-up\n", KEY_DOWN_PIN);
#endif

#if defined KEY_LEFT_PIN
    DEV_GPIO_Mode(KEY_LEFT_PIN, 1);
    DEV_Digital_Write(KEY_LEFT_PIN, 1); // 设置为高电平（上拉）
    printf("KEY_LEFT_PIN (%d) initialized as input with pull-up\n", KEY_LEFT_PIN);
#endif

#if defined KEY_RIGHT_PIN
    DEV_GPIO_Mode(KEY_RIGHT_PIN, 1);
    DEV_Digital_Write(KEY_RIGHT_PIN, 1); // 设置为高电平（上拉）
    printf("KEY_RIGHT_PIN (%d) initialized as input with pull-up\n", KEY_RIGHT_PIN);
#endif
    
    // LCD 控制引脚初始化 (输出模式)
    DEV_GPIO_Mode(LCD_CS_PIN, 1);      // 片选信号
    DEV_GPIO_Mode(LCD_RST_PIN, 1);     // 复位信号
    DEV_GPIO_Mode(LCD_DC_PIN, 1);      // 数据/命令选择
    DEV_GPIO_Mode(LCD_BL_PIN, 1);      // 背光控制
    
    // 初始化 LCD 控制信号状态
    LCD_CS(1);      // 取消片选
    LCD_RST(1);     // 释放复位
    LCD_DC(1);      // 默认数据模式
    LCD_BL(1);      // 开启背光
    
    printf("LCD pins initialized\n");
}

/******************************************************************************
function:	Module Initialize, the library and initialize the pins, SPI protocol
parameter:
Info:
******************************************************************************/
UBYTE DEV_ModuleInit(void)
{
    DEV_GPIO_Init();
    return 0;
}

/******************************************************************************
function:	Module cleanup, release GPIO resources
parameter:
Info:
******************************************************************************/
void DEV_ModuleExit(void)
{
    printf("Cleaning up GPIO resources...\n");
    
#if defined KEY_UP_PIN
    libgpio_unexport(KEY_UP_PIN);
#endif

#if defined KEY_DOWN_PIN
    libgpio_unexport(KEY_DOWN_PIN);
#endif

#if defined KEY_LEFT_PIN
    libgpio_unexport(KEY_LEFT_PIN);
#endif

#if defined KEY_RIGHT_PIN
    libgpio_unexport(KEY_RIGHT_PIN);
#endif

    libgpio_unexport(LCD_CS_PIN);
    libgpio_unexport(LCD_RST_PIN);
    libgpio_unexport(LCD_DC_PIN);
    libgpio_unexport(LCD_BL_PIN);
    
    // 清理 libgpio 库
    libgpio_deinit();
    
    printf("GPIO cleanup completed\n");
}
