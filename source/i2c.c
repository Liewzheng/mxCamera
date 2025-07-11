/**
 * @file i2c.c
 * @brief INA219 电池电量检测模块
 *
 * 功能：
 * - INA219 设备初始化和配置
 * - 电压、电流、功率实时读取
 * - 电池电量百分比计算
 * - 5V 系统健康状态分析
 * - I2C 通信管理
 */

#include "mxCamera.h"
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <sys/ioctl.h>

// ============================================================================
// INA219 寄存器和配置常量
// ============================================================================

#define INA219_DEVICE_ADDRESS 0x40 // I2C 设备地址
#define INA219_I2C_BUS 3           // I2C 总线号

// INA219 寄存器地址
#define INA219_REG_CONFIG 0x00
#define INA219_REG_SHUNT_VOLTAGE 0x01
#define INA219_REG_BUS_VOLTAGE 0x02
#define INA219_REG_POWER 0x03
#define INA219_REG_CURRENT 0x04
#define INA219_REG_CALIBRATION 0x05

// 配置值和校准值（参考 i2c.py）
#define INA219_CONFIG_VALUE 0x1E9F 
#define INA219_SHUNT_RESISTOR 0.5f // 分流电阻值（欧姆）- 参考 Python 代码

// 校准值计算 - 参考 Python 代码的 calculate_calibration 函数
#define INA219_CALIBRATION_VALUE 0x029F // 根据Python公式计算的校准值

// LSB
#define INA219_CURRENT_LSB 0.0001f 

// 电池电量计算参数
#define BATTERY_VOLTAGE_MIN 4.5f          // 最低电压 (V)
#define BATTERY_VOLTAGE_MAX 5.25f         // 最高电压 (V)
#define BATTERY_VOLTAGE_OPTIMAL_MIN 4.75f // 优化范围最低电压 (V)
#define BATTERY_VOLTAGE_OPTIMAL_MAX 5.2f  // 优化范围最高电压 (V)

// ============================================================================
// 全局变量
// ============================================================================

static int i2c_fd = -1;                                       // I2C 文件描述符
static int ina219_initialized = 0;                            // INA219 初始化状态
static float current_battery_percentage = 0.0f;               // 当前电池电量百分比
static float current_voltage = 0.0f;                          // 当前电压
static float current_current = 0.0f;                          // 当前电流
static float current_power = 0.0f;                            // 当前功率
static float last_stable_percentage = 0.0f;                   // 上次稳定的电池百分比，用于滤波
static pthread_mutex_t i2c_mutex = PTHREAD_MUTEX_INITIALIZER; // I2C 访问互斥锁

// ============================================================================
// I2C 通信底层函数
// ============================================================================

/**
 * @brief 向 INA219 写入字寄存器
 * @param reg 寄存器地址
 * @param value 要写入的值
 * @return 0 成功，-1 失败
 */
static int ina219_write_register(uint8_t reg, uint16_t value)
{
    if (i2c_fd < 0)
    {
        printf("Error: I2C device not opened\n");
        return -1;
    }

    // INA219 使用大端序 (MSB first)
    uint8_t buffer[3] = {reg, (value >> 8) & 0xFF, value & 0xFF};
    
    printf("Debug: Writing to reg 0x%02X: value=0x%04X, bytes=[0x%02X, 0x%02X]\n",
           reg, value, buffer[1], buffer[2]);
    
    if (write(i2c_fd, buffer, 3) != 3)
    {
        printf("Error: Failed to write to INA219 register 0x%02X: %s\n", reg, strerror(errno));
        return -1;
    }

    return 0;
}

/**
 * @brief 从 INA219 读取字寄存器
 * @param reg 寄存器地址
 * @param value 输出读取的值
 * @return 0 成功，-1 失败
 */
static int ina219_read_register(uint8_t reg, uint16_t *value)
{
    if (i2c_fd < 0 || !value)
    {
        printf("Error: Invalid parameters for INA219 read\n");
        return -1;
    }

    // 写入寄存器地址
    if (write(i2c_fd, &reg, 1) != 1)
    {
        printf("Error: Failed to write register address 0x%02X: %s\n", reg, strerror(errno));
        return -1;
    }

    // 读取2字节数据
    uint8_t buffer[2];
    if (read(i2c_fd, buffer, 2) != 2)
    {
        printf("Error: Failed to read from INA219 register 0x%02X: %s\n", reg, strerror(errno));
        return -1;
    }

    *value = (buffer[0] << 8) | buffer[1];

    printf("Debug: Read from reg 0x%02X: bytes=[0x%02X, 0x%02X], value=0x%04X\n",
           reg, buffer[0], buffer[1], *value);

    return 0;
}

// ============================================================================
// INA219 设备管理函数
// ============================================================================

/**
 * @brief 初始化 INA219 设备
 * @return 0 成功，-1 失败
 */
int init_ina219(void)
{
    pthread_mutex_lock(&i2c_mutex);

    // 打开 I2C 设备文件
    char i2c_device[32];
    snprintf(i2c_device, sizeof(i2c_device), "/dev/i2c-%d", INA219_I2C_BUS);

    i2c_fd = open(i2c_device, O_RDWR);
    if (i2c_fd < 0)
    {
        printf("Error: Failed to open I2C device %s: %s\n", i2c_device, strerror(errno));
        pthread_mutex_unlock(&i2c_mutex);
        return -1;
    }

    // 设置 I2C 从设备地址
    if (ioctl(i2c_fd, I2C_SLAVE, INA219_DEVICE_ADDRESS) < 0)
    {
        printf("Error: Failed to set I2C slave address 0x%02X: %s\n",
               INA219_DEVICE_ADDRESS, strerror(errno));
        close(i2c_fd);
        i2c_fd = -1;
        pthread_mutex_unlock(&i2c_mutex);
        return -1;
    }

    printf("INA219 I2C communication established on bus %d, address 0x%02X\n",
           INA219_I2C_BUS, INA219_DEVICE_ADDRESS);

    // 重置设备
    printf("Resetting INA219...\n");
    if (ina219_write_register(INA219_REG_CONFIG, 0x8000) != 0)
    {
        printf("Error: Failed to reset INA219\n");
        close(i2c_fd);
        i2c_fd = -1;
        pthread_mutex_unlock(&i2c_mutex);
        return -1;
    }

    usleep(100000); // 增加重置等待时间到50ms

    // 配置 INA219
    printf("Setting INA219 configuration value: 0x%04X\n", INA219_CONFIG_VALUE);
    if (ina219_write_register(INA219_REG_CONFIG, INA219_CONFIG_VALUE) != 0)
    {
        printf("Error: Failed to set INA219 configuration\n");
        close(i2c_fd);
        i2c_fd = -1;
        pthread_mutex_unlock(&i2c_mutex);
        return -1;
    }


    // 设置校准值
    printf("Setting INA219 calibration value: 0x%04X\n", INA219_CALIBRATION_VALUE);
    if (ina219_write_register(INA219_REG_CALIBRATION, INA219_CALIBRATION_VALUE) != 0)
    {
        printf("Error: Failed to set INA219 calibration, continuing anyway\n");
        close(i2c_fd);
        i2c_fd = -1;
        pthread_mutex_unlock(&i2c_mutex);
        return -1;
    }

    // 测试读取 INA219_CALIBRATION_VALUE 用以验证
    uint16_t config_value;
    if (ina219_read_register(INA219_REG_CONFIG, &config_value) != 0)
    {
        printf("Error: Failed to read INA219 configuration register\n");
        close(i2c_fd);
        i2c_fd = -1;
        pthread_mutex_unlock(&i2c_mutex);
        return -1;
    }

    if (config_value != INA219_CONFIG_VALUE)
    {
        printf("Warning: INA219 configuration mismatch! Expected 0x%04X, got 0x%04X\n",
               INA219_CONFIG_VALUE, config_value);
    }
    else
    {
        printf("INA219 configuration verified successfully\n");
    }

    // 测试读取 INA219_REG_CONFIG 用以验证
    uint16_t calibration_value;
    if (ina219_read_register(INA219_REG_CALIBRATION, &calibration_value) != 0)
    {
        printf("Error: Failed to read INA219 calibration register\n");
        close(i2c_fd);
        i2c_fd = -1;
        pthread_mutex_unlock(&i2c_mutex);
        return -1;
    }

    if (calibration_value != INA219_CALIBRATION_VALUE)
    {
        printf("Warning: INA219 calibration mismatch! Expected 0x%04X, got 0x%04X\n",
               INA219_CALIBRATION_VALUE, calibration_value);
    }
    else
    {
        printf("INA219 calibration verified successfully\n");
    }

    ina219_initialized = 1; // 仍然标记为已初始化，以便测试
    pthread_mutex_unlock(&i2c_mutex);
    return 0;
}

/**
 * @brief 清理 INA219 设备
 */
void cleanup_ina219(void)
{
    pthread_mutex_lock(&i2c_mutex);

    if (i2c_fd >= 0)
    {
        close(i2c_fd);
        i2c_fd = -1;
    }

    ina219_initialized = 0;
    printf("INA219 device cleaned up\n");

    pthread_mutex_unlock(&i2c_mutex);
}

/**
 * @brief 读取 INA219 原始数据（参考 i2c.py 的转换逻辑）
 * @param bus_voltage 输出总线电压 (V)
 * @param shunt_voltage 输出分流电压 (V)
 * @param current 输出电流 (A)
 * @param power 输出功率 (W)
 * @return 0 成功，-1 失败
 */
int read_ina219_data(float *bus_voltage, float *shunt_voltage, float *current, float *power)
{
    if (!ina219_initialized || !bus_voltage || !shunt_voltage || !current || !power)
    {
        return -1;
    }

    pthread_mutex_lock(&i2c_mutex);

    // 读取原始寄存器值
    uint16_t bus_voltage_raw;
    uint16_t shunt_voltage_raw; // 先读取为无符号，后面处理符号
    uint16_t current_raw;       // 先读取为无符号，后面处理符号
    uint16_t power_raw;

    int16_t bus_voltage_raw_signed;
    int16_t shunt_voltage_raw_signed;

    int ret = 0;

    if (ina219_read_register(INA219_REG_BUS_VOLTAGE, &bus_voltage_raw) != 0 ||
        ina219_read_register(INA219_REG_SHUNT_VOLTAGE, &shunt_voltage_raw) != 0 ||
        ina219_read_register(INA219_REG_CURRENT, &current_raw) != 0 ||
        ina219_read_register(INA219_REG_POWER, &power_raw) != 0)
    {
        printf("Error: Failed to read INA219 registers\n");
        ret = -1;
    }
    else
    {
        if (bus_voltage_raw > 32767)
        {
            bus_voltage_raw_signed = bus_voltage_raw - 65536; // 处理有符号整数
        }
        else
        {
            bus_voltage_raw_signed = (int16_t)bus_voltage_raw; // 直接转换为有符号整数
        }
        if (shunt_voltage_raw > 32767)
        {
            shunt_voltage_raw_signed = shunt_voltage_raw - 65536; // 处理有符号整数
        }
        else
        {
            shunt_voltage_raw_signed = (int16_t)shunt_voltage_raw; // 直接转换为有符号整数
        }

        // 分流电压：LSB = 10μV (有符号值) - 参考 Python 代码
        *shunt_voltage = shunt_voltage_raw_signed * 0.00001f; // 转换为 V

        // 检查 bus_voltage_raw 的 ready 位 和 溢出 位
        if ((bus_voltage_raw_signed & 0x02) == 0x02)
        { // 检查转换就绪标志
            if ((bus_voltage_raw_signed & 0x01) == 0)
            { // 检查溢出标志
                *bus_voltage = (bus_voltage_raw_signed >> 3) * 0.004f; // 转换为 V
            }
            else
            {
                printf("Warning: Bus voltage overflow detected\n");
                *bus_voltage = -1.0f; // 溢出时设置为无效值
            }
        }
        else
        {
            printf("Warning: Bus voltage conversion not ready\n");
            *bus_voltage = -1.0f; // 未准备好时设置为无效值
        }

        *current = (int16_t)current_raw * INA219_CURRENT_LSB; // 转换为 A
        *power = power_raw * 20.0f * INA219_CURRENT_LSB; // 转换为 W

        printf("Debug: Converted values - Bus: %.3fV, Shunt: %.3fmV, Current: %.3fA, Power: %.3fW\n",
               (double)*bus_voltage, (double)(*shunt_voltage * 1000.0f), (double)*current, (double)*power);
    }

    pthread_mutex_unlock(&i2c_mutex);
    return ret;
}

/**
 * @brief 计算电池电量百分比（优化的5V系统算法，减少敏感性）
 * @param voltage 当前电压 (V)
 * @return 电池电量百分比 (0.0-100.0)
 */
static float calculate_battery_percentage(float voltage)
{
    if (voltage <= 0)
    {
        return 0.0f;
    }

    // 优化的5V系统电池百分比算法
    // 基于实际观察的电压范围，减少小电压变化的影响
    float system_percent = 0.0f;

    if (voltage >= 5.0f)
    {
        // 高电压范围：5.0V - 5.25V → 80% - 100%
        if (voltage >= 5.25f)
        {
            system_percent = 100.0f;
        }
        else
        {
            system_percent = 80.0f + ((voltage - 5.0f) / (5.25f - 5.0f)) * 20.0f;
        }
    }
    else if (voltage >= 4.9f)
    {
        // 良好范围：4.9V - 5.0V → 60% - 80%
        system_percent = 60.0f + ((voltage - 4.9f) / (5.0f - 4.9f)) * 20.0f;
    }
    else if (voltage >= 4.8f)
    {
        // 正常范围：4.8V - 4.9V → 40% - 60%
        system_percent = 40.0f + ((voltage - 4.8f) / (4.9f - 4.8f)) * 20.0f;
    }
    else if (voltage >= 4.7f)
    {
        // 边际范围：4.7V - 4.8V → 20% - 40%
        system_percent = 20.0f + ((voltage - 4.7f) / (4.8f - 4.7f)) * 20.0f;
    }
    else if (voltage >= 4.5f)
    {
        // 低电压范围：4.5V - 4.7V → 5% - 20%
        system_percent = 5.0f + ((voltage - 4.5f) / (4.7f - 4.5f)) * 15.0f;
    }
    else
    {
        // 严重低电压：< 4.5V → 0% - 5%
        if (voltage >= 4.0f)
        {
            system_percent = ((voltage - 4.0f) / (4.5f - 4.0f)) * 5.0f;
        }
        else
        {
            system_percent = 0.0f;
        }
    }

    // 限制范围并添加调试信息
    system_percent = fmaxf(0.0f, fminf(100.0f, system_percent));

    printf("Debug: Battery calculation - voltage=%.3fV, percentage=%.1f%%\n",
           (double)voltage, (double)system_percent);

    return system_percent;
}

/**
 * @brief 更新电池状态（带滤波，减少频繁变化）
 * @return 0 成功，-1 失败
 */
int update_battery_status(void)
{
    if (!ina219_initialized)
    {
        printf("Debug: INA219 not initialized, cannot update battery status\n");
        return -1;
    }

    float bus_voltage, shunt_voltage, current, power;

    if (read_ina219_data(&bus_voltage, &shunt_voltage, &current, &power) == 0)
    {
        // 更新全局变量
        current_voltage = bus_voltage;
        current_current = current;
        current_power = power;

        float new_percentage = calculate_battery_percentage(bus_voltage);

        // 应用简单的滤波：只有当变化超过2.5%时才更新显示的百分比
        // 这样可以减少因为小的电压波动导致的百分比频繁变化
        float percentage_change = fabsf(new_percentage - last_stable_percentage);

        if (last_stable_percentage == 0.0f || percentage_change >= 2.5f)
        {
            // 首次初始化或变化超过2.5%时更新
            current_battery_percentage = new_percentage;
            last_stable_percentage = new_percentage;
            printf("Debug: Battery percentage updated to %.1f%% (change: %.1f%%)\n",
                   (double)current_battery_percentage, (double)percentage_change);
        }
        else
        {
            // 变化小于2.5%时保持当前显示值，但记录实际值用于调试
            printf("Debug: Battery percentage filtered - actual: %.1f%%, displayed: %.1f%% (change: %.1f%% < 2.5%%)\n",
                   (double)new_percentage, (double)current_battery_percentage, (double)percentage_change);
        }

        printf("Debug: INA219 readings - Voltage: %.3fV, Current: %.3fA, Power: %.3fW, Battery: %.1f%%\n",
               (double)bus_voltage, (double)current, (double)power, (double)current_battery_percentage);

        return 0;
    }
    else
    {
        printf("Debug: Failed to read INA219 data\n");
    }

    return -1;
}

/**
 * @brief 获取当前电池电量百分比
 * @return 电池电量百分比 (0.0-100.0)
 */
float get_battery_percentage(void)
{
    return current_battery_percentage;
}

/**
 * @brief 获取当前电压
 * @return 电压值 (V)
 */
float get_battery_voltage(void)
{
    return current_voltage;
}

/**
 * @brief 获取当前电流
 * @return 电流值 (A)
 */
float get_battery_current(void)
{
    return current_current;
}

/**
 * @brief 获取当前功率
 * @return 功率值 (W)
 */
float get_battery_power(void)
{
    return current_power;
}

/**
 * @brief 检查 INA219 是否已初始化
 * @return 1 已初始化，0 未初始化
 */
int is_ina219_initialized(void)
{
    return ina219_initialized;
}
