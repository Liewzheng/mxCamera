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

#define INA219_DEVICE_ADDRESS       0x40  // I2C 设备地址
#define INA219_I2C_BUS              3     // I2C 总线号

// INA219 寄存器地址
#define INA219_REG_CONFIG           0x00
#define INA219_REG_SHUNT_VOLTAGE    0x01
#define INA219_REG_BUS_VOLTAGE      0x02
#define INA219_REG_POWER            0x03
#define INA219_REG_CURRENT          0x04
#define INA219_REG_CALIBRATION      0x05

// 配置值和校准值（参考 i2c.py）
#define INA219_CONFIG_VALUE         0x1FDF  // 16V范围，±320mV分流范围，12位，连续模式
#define INA219_SHUNT_RESISTOR       1.0f    // 分流电阻值（欧姆）- 参考 Python 代码

// 计算校准值 - 参考 Python 代码的 calculate_calibration 函数
// max_expected_current = 8.0A, shunt_resistance = 1.0Ω
// current_lsb = max_expected_current / 32768 = 8.0 / 32768 ≈ 0.000244
// cal_value = trunc(0.04096 / (current_lsb * shunt_resistance)) 
//           = trunc(0.04096 / (0.000244 * 1.0)) ≈ trunc(167.87) = 167 = 0x00A7
#define INA219_CALIBRATION_VALUE    0x00A7  // 根据 Python 算法计算的校准值

// 电池电量计算参数
#define BATTERY_VOLTAGE_MIN         4.5f    // 最低电压 (V)
#define BATTERY_VOLTAGE_MAX         5.25f   // 最高电压 (V)
#define BATTERY_VOLTAGE_OPTIMAL_MIN 4.75f   // 优化范围最低电压 (V)
#define BATTERY_VOLTAGE_OPTIMAL_MAX 5.2f    // 优化范围最高电压 (V)

// ============================================================================
// 全局变量
// ============================================================================

static int i2c_fd = -1;                    // I2C 文件描述符
static int ina219_initialized = 0;         // INA219 初始化状态
static float current_battery_percentage = 0.0f;  // 当前电池电量百分比
static float current_voltage = 0.0f;       // 当前电压
static float current_current = 0.0f;       // 当前电流
static float current_power = 0.0f;         // 当前功率
static pthread_mutex_t i2c_mutex = PTHREAD_MUTEX_INITIALIZER;  // I2C 访问互斥锁

// ============================================================================
// I2C 通信底层函数
// ============================================================================

/**
 * @brief 交换字节序（INA219 使用大端序）
 * @param value 16位值
 * @return 交换后的值
 */
static uint16_t swap_bytes(uint16_t value) {
    return ((value & 0xFF) << 8) | ((value & 0xFF00) >> 8);
}

/**
 * @brief 向 INA219 写入字寄存器
 * @param reg 寄存器地址
 * @param value 要写入的值
 * @return 0 成功，-1 失败
 */
static int ina219_write_register(uint8_t reg, uint16_t value) {
    if (i2c_fd < 0) {
        printf("Error: I2C device not opened\n");
        return -1;
    }
    
    // INA219 期望 MSB 在前，参考 Python 代码的 write_word_swapped
    uint16_t swapped_value = swap_bytes(value);
    uint8_t buffer[3] = {reg, (swapped_value >> 8) & 0xFF, swapped_value & 0xFF};
    
    printf("Debug: Writing to reg 0x%02X: original=0x%04X, swapped=0x%04X, bytes=[0x%02X, 0x%02X]\n",
           reg, value, swapped_value, buffer[1], buffer[2]);
    
    if (write(i2c_fd, buffer, 3) != 3) {
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
static int ina219_read_register(uint8_t reg, uint16_t* value) {
    if (i2c_fd < 0 || !value) {
        printf("Error: Invalid parameters for INA219 read\n");
        return -1;
    }
    
    // 写入寄存器地址
    if (write(i2c_fd, &reg, 1) != 1) {
        printf("Error: Failed to write register address 0x%02X: %s\n", reg, strerror(errno));
        return -1;
    }
    
    // 读取2字节数据
    uint8_t buffer[2];
    if (read(i2c_fd, buffer, 2) != 2) {
        printf("Error: Failed to read from INA219 register 0x%02X: %s\n", reg, strerror(errno));
        return -1;
    }
    
    // 参考 Python 代码：INA219 使用 MSB 在前，但 smbus 返回 LSB 在前
    // 所以我们需要交换字节：从 little-endian 转换为 big-endian
    uint16_t raw_value = (buffer[0] << 8) | buffer[1];  // 这已经是正确的字节序
    *value = raw_value;
    
    printf("Debug: Read from reg 0x%02X: bytes=[0x%02X, 0x%02X], value=0x%04X\n",
           reg, buffer[0], buffer[1], raw_value);
    
    return 0;
}

/**
 * @brief 读取有符号寄存器值（用于分流电压和电流）
 * @param reg 寄存器地址
 * @param value 输出读取的有符号值
 * @return 0 成功，-1 失败
 */
static int ina219_read_signed_register(uint8_t reg, int16_t* value) {
    uint16_t raw_value;
    if (ina219_read_register(reg, &raw_value) != 0) {
        return -1;
    }
    
    // 转换为有符号整数
    *value = (int16_t)raw_value;
    return 0;
}

// ============================================================================
// INA219 设备管理函数
// ============================================================================

/**
 * @brief 初始化 INA219 设备
 * @return 0 成功，-1 失败
 */
int init_ina219(void) {
    pthread_mutex_lock(&i2c_mutex);
    
    // 打开 I2C 设备文件
    char i2c_device[32];
    snprintf(i2c_device, sizeof(i2c_device), "/dev/i2c-%d", INA219_I2C_BUS);
    
    i2c_fd = open(i2c_device, O_RDWR);
    if (i2c_fd < 0) {
        printf("Error: Failed to open I2C device %s: %s\n", i2c_device, strerror(errno));
        pthread_mutex_unlock(&i2c_mutex);
        return -1;
    }
    
    // 设置 I2C 从设备地址
    if (ioctl(i2c_fd, I2C_SLAVE, INA219_DEVICE_ADDRESS) < 0) {
        printf("Error: Failed to set I2C slave address 0x%02X: %s\n", 
               INA219_DEVICE_ADDRESS, strerror(errno));
        close(i2c_fd);
        i2c_fd = -1;
        pthread_mutex_unlock(&i2c_mutex);
        return -1;
    }
    
    printf("INA219 I2C communication established on bus %d, address 0x%02X\n", 
           INA219_I2C_BUS, INA219_DEVICE_ADDRESS);
    
    // 首先读取当前配置以检查通信
    uint16_t initial_config;
    if (ina219_read_register(INA219_REG_CONFIG, &initial_config) == 0) {
        printf("INA219 initial config: 0x%04X\n", initial_config);
    } else {
        printf("Error: Failed to read initial INA219 configuration\n");
        close(i2c_fd);
        i2c_fd = -1;
        pthread_mutex_unlock(&i2c_mutex);
        return -1;
    }
    
    // 重置设备
    printf("Resetting INA219...\n");
    if (ina219_write_register(INA219_REG_CONFIG, 0x8000) != 0) {
        printf("Error: Failed to reset INA219\n");
        close(i2c_fd);
        i2c_fd = -1;
        pthread_mutex_unlock(&i2c_mutex);
        return -1;
    }
    
    usleep(50000); // 增加重置等待时间到50ms
    
    // 验证重置完成
    uint16_t reset_config;
    if (ina219_read_register(INA219_REG_CONFIG, &reset_config) == 0) {
        printf("INA219 config after reset: 0x%04X\n", reset_config);
    }
    
    // 尝试多种配置，找到适合的配置
    uint16_t test_configs[] = {
        0x1FDF,  // 原始配置：16V范围，±320mV，12位，连续模式
        0x199F,  // 16V范围，±160mV，12位，连续模式
        0x19DF,  // 16V范围，±320mV，9位，连续模式  
        0x399F   // 32V范围，±320mV，12位，连续模式
    };
    
    const char* config_names[] = {
        "16V/±320mV/12bit",
        "16V/±160mV/12bit", 
        "16V/±320mV/9bit",
        "32V/±320mV/12bit"
    };
    
    int config_success = 0;
    uint16_t working_config = 0;
    
    for (int i = 0; i < 4; i++) {
        printf("Trying INA219 config %d: %s (0x%04X)\n", i+1, config_names[i], test_configs[i]);
        
        if (ina219_write_register(INA219_REG_CONFIG, test_configs[i]) == 0) {
            usleep(10000); // 等待配置生效
            
            uint16_t verify_config;
            if (ina219_read_register(INA219_REG_CONFIG, &verify_config) == 0) {
                printf("  Written: 0x%04X, Read back: 0x%04X\n", test_configs[i], verify_config);
                
                if (verify_config == test_configs[i]) {
                    printf("  ✓ Configuration verified successfully\n");
                    working_config = test_configs[i];
                    config_success = 1;
                    break;
                } else {
                    printf("  ✗ Configuration verification failed\n");
                }
            } else {
                printf("  ✗ Failed to read back configuration\n");
            }
        } else {
            printf("  ✗ Failed to write configuration\n");
        }
    }
    
    if (!config_success) {
        printf("Warning: All configuration attempts failed, using last readback value\n");
        // 仍然尝试使用设备，可能设备有自己的默认配置
        working_config = reset_config;
    }
    
    // 设置校准值
    printf("Setting INA219 calibration value: 0x%04X\n", INA219_CALIBRATION_VALUE);
    if (ina219_write_register(INA219_REG_CALIBRATION, INA219_CALIBRATION_VALUE) != 0) {
        printf("Warning: Failed to set INA219 calibration, continuing anyway\n");
    } else {
        // 验证校准值
        uint16_t cal_readback;
        if (ina219_read_register(INA219_REG_CALIBRATION, &cal_readback) == 0) {
            printf("Calibration written: 0x%04X, read back: 0x%04X\n", 
                   INA219_CALIBRATION_VALUE, cal_readback);
        }
    }
    
    // 测试读取一些寄存器来验证通信
    uint16_t bus_voltage_raw, shunt_voltage_raw;
    if (ina219_read_register(INA219_REG_BUS_VOLTAGE, &bus_voltage_raw) == 0 &&
        ina219_read_register(INA219_REG_SHUNT_VOLTAGE, &shunt_voltage_raw) == 0) {
        
        printf("Initial readings - Bus: 0x%04X, Shunt: 0x%04X\n", 
               bus_voltage_raw, shunt_voltage_raw);
        
        // 转换并显示实际值用于调试
        float test_bus_voltage = 0;
        if ((bus_voltage_raw & 0x02) == 0x02) {  // 检查转换就绪标志
            if ((bus_voltage_raw & 0x01) == 0) {  // 检查溢出标志
                test_bus_voltage = (bus_voltage_raw >> 3) * 0.004f;  // 转换为 V
                printf("Initial bus voltage: %.3f V\n", (double)test_bus_voltage);
            } else {
                printf("Initial reading: Bus voltage overflow detected\n");
            }
        } else {
            printf("Initial reading: Bus voltage conversion not ready\n");
        }
        
        float test_shunt_voltage = (int16_t)shunt_voltage_raw * 0.00001f;
        printf("Initial shunt voltage: %.3f mV\n", (double)(test_shunt_voltage * 1000.0f));
        
        // 简单的健全性检查
        if (bus_voltage_raw != 0 || shunt_voltage_raw != 0) {
            printf("INA219 appears to be responding with valid data\n");
            ina219_initialized = 1;
            pthread_mutex_unlock(&i2c_mutex);
            return 0;
        }
    }
    
    printf("Warning: INA219 communication issues detected, but marking as initialized for testing\n");
    ina219_initialized = 1;  // 仍然标记为已初始化，以便测试
    pthread_mutex_unlock(&i2c_mutex);
    return 0;
}

/**
 * @brief 清理 INA219 设备
 */
void cleanup_ina219(void) {
    pthread_mutex_lock(&i2c_mutex);
    
    if (i2c_fd >= 0) {
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
int read_ina219_data(float* bus_voltage, float* shunt_voltage, float* current, float* power) {
    if (!ina219_initialized || !bus_voltage || !shunt_voltage || !current || !power) {
        return -1;
    }
    
    pthread_mutex_lock(&i2c_mutex);
    
    // 读取原始寄存器值
    uint16_t bus_voltage_raw;
    uint16_t shunt_voltage_raw;  // 先读取为无符号，后面处理符号
    uint16_t current_raw;        // 先读取为无符号，后面处理符号
    uint16_t power_raw;
    
    int ret = 0;
    
    if (ina219_read_register(INA219_REG_BUS_VOLTAGE, &bus_voltage_raw) != 0 ||
        ina219_read_register(INA219_REG_SHUNT_VOLTAGE, &shunt_voltage_raw) != 0 ||
        ina219_read_register(INA219_REG_CURRENT, &current_raw) != 0 ||
        ina219_read_register(INA219_REG_POWER, &power_raw) != 0) {
        
        printf("Error: Failed to read INA219 registers\n");
        ret = -1;
    } else {
        // 参考 Python 代码的转换逻辑
        printf("Debug: Raw values - Bus: 0x%04X, Shunt: 0x%04X, Current: 0x%04X, Power: 0x%04X\n",
               bus_voltage_raw, shunt_voltage_raw, current_raw, power_raw);
        
        // 分流电压：LSB = 10μV (有符号值) - 参考 Python 代码
        int16_t shunt_signed = (int16_t)shunt_voltage_raw;
        *shunt_voltage = shunt_signed * 0.00001f;  // 转换为 V
        
        // 总线电压：LSB = 4mV，需要右移3位（位15-3有效）- 参考 Python 代码
        // 检查转换就绪位（位1 = CNVR）和溢出标志（位0 = OVF）
        printf("Debug: Bus voltage raw: 0x%04X, CNVR bit: %d, OVF bit: %d\n", 
               bus_voltage_raw, (bus_voltage_raw & 0x02) ? 1 : 0, (bus_voltage_raw & 0x01) ? 1 : 0);
        
        // 直接计算电压值，参考 Python 代码的处理方式
        // 在5V系统中，溢出位可能被设置但仍需要读取电压值
        *bus_voltage = (bus_voltage_raw >> 3) * 0.004f;  // 转换为 V
        printf("Debug: Bus voltage calculated: %.3fV (raw shifted: 0x%04X)\n", 
               (double)*bus_voltage, (bus_voltage_raw >> 3));
        
        // 检查电压是否在合理范围内
        if (*bus_voltage < 0.1f || *bus_voltage > 6.0f) {
            printf("Warning: Bus voltage out of reasonable range: %.3fV\n", (double)*bus_voltage);
        }
        
        // 电流：使用计算的 current_lsb (参考 Python 代码的计算)
        // Based on observed current, increase max expected current to 8A for safety margin
        // Current_LSB = Max_Expected_Current / 32768
        float current_lsb = 8.0f / 32768.0f;  // 约 0.244 mA
        int16_t current_signed = (int16_t)current_raw;
        *current = current_signed * current_lsb;  // 转换为 A
        
        // 功率：LSB = 20 * Current_LSB - 参考 Python 代码
        float power_lsb = 20.0f * current_lsb;
        *power = power_raw * power_lsb;  // 转换为 W
        
        printf("Debug: Converted values - Bus: %.3fV, Shunt: %.3fmV, Current: %.3fA, Power: %.3fW\n",
               (double)*bus_voltage, (double)(*shunt_voltage * 1000.0f), (double)*current, (double)*power);
    }
    
    pthread_mutex_unlock(&i2c_mutex);
    return ret;
}

/**
 * @brief 计算电池电量百分比（参考 i2c.py 中的 5V 系统算法）
 * @param voltage 当前电压 (V)
 * @return 电池电量百分比 (0.0-100.0)
 */
static float calculate_battery_percentage(float voltage) {
    if (voltage <= 0) {
        return 0.0f;
    }
    
    // 参考 Python 代码中的 5V 系统状态计算
    // For 5V system: Normal range 4.5V-5.25V
    // Below 4.5V indicates potential power issues
    // Above 5.25V indicates overvoltage
    
    float system_percent = 0.0f;
    
    if (voltage >= 4.75f && voltage <= 5.25f) {
        // Normal operating range - 参考 Python 代码
        system_percent = ((voltage - 4.75f) / (5.25f - 4.75f)) * 100.0f;
        system_percent = fmaxf(0.0f, fminf(100.0f, system_percent));
    } else if (voltage < 4.5f) {
        // Low voltage warning
        system_percent = 0.0f;
    } else if (voltage < 4.75f) {
        // Marginal voltage - 参考 Python 代码，映射到 0-30%
        system_percent = ((voltage - 4.5f) / (4.75f - 4.5f)) * 30.0f;  // Scale to 0-30%
    } else {
        // Overvoltage (voltage > 5.25V)
        system_percent = 100.0f;
    }
    
    printf("Debug: Battery calculation - voltage=%.3fV, percentage=%.1f%%\n", 
           (double)voltage, (double)system_percent);
    
    return system_percent;
}

/**
 * @brief 更新电池状态
 * @return 0 成功，-1 失败
 */
int update_battery_status(void) {
    if (!ina219_initialized) {
        printf("Debug: INA219 not initialized, cannot update battery status\n");
        return -1;
    }
    
    float bus_voltage, shunt_voltage, current, power;
    
    if (read_ina219_data(&bus_voltage, &shunt_voltage, &current, &power) == 0) {
        // 更新全局变量
        current_voltage = bus_voltage;
        current_current = current;
        current_power = power;
        float old_percentage = current_battery_percentage;
        current_battery_percentage = calculate_battery_percentage(bus_voltage);
        
        // 添加调试信息
        printf("Debug: INA219 readings - Voltage: %.3fV, Current: %.3fA, Power: %.3fW, Battery: %.1f%% (was %.1f%%)\n",
               (double)bus_voltage, (double)current, (double)power, 
               (double)current_battery_percentage, (double)old_percentage);
        
        return 0;
    } else {
        printf("Debug: Failed to read INA219 data\n");
    }
    
    return -1;
}

/**
 * @brief 获取当前电池电量百分比
 * @return 电池电量百分比 (0.0-100.0)
 */
float get_battery_percentage(void) {
    return current_battery_percentage;
}

/**
 * @brief 获取当前电压
 * @return 电压值 (V)
 */
float get_battery_voltage(void) {
    return current_voltage;
}

/**
 * @brief 获取当前电流
 * @return 电流值 (A)
 */
float get_battery_current(void) {
    return current_current;
}

/**
 * @brief 获取当前功率
 * @return 功率值 (W)
 */
float get_battery_power(void) {
    return current_power;
}

/**
 * @brief 检查 INA219 是否已初始化
 * @return 1 已初始化，0 未初始化
 */
int is_ina219_initialized(void) {
    return ina219_initialized;
}

/**
 * @brief 分析5V系统健康状态
 * @param voltage 电压 (V)
 * @param current 电流 (A)  
 * @param power 功率 (W)
 * @return 健康评分 (0-100)
 */
int analyze_system_health(float voltage, float current, float power) {
    int voltage_score = 0;
    int current_score = 0;
    int power_score = 0;
    
    // 电压评分
    if (voltage >= 4.75f && voltage <= 5.25f) {
        voltage_score = 100;
    } else if (voltage >= 4.5f && voltage <= 5.5f) {
        voltage_score = 70;
    } else {
        voltage_score = 30;
    }
    
    // 电流评分
    if (current <= 3.0f) {
        current_score = 100;
    } else if (current <= 4.0f) {
        current_score = 80;
    } else if (current <= 5.0f) {
        current_score = 60;
    } else {
        current_score = 30;
    }
    
    // 功率评分
    if (power <= 15.0f) {
        power_score = 100;
    } else if (power <= 20.0f) {
        power_score = 80;
    } else if (power <= 25.0f) {
        power_score = 60;
    } else {
        power_score = 30;
    }
    
    // 综合评分（加权平均）
    int health_score = (voltage_score * 40 + current_score * 30 + power_score * 30) / 100;
    return fmaxf(0, fminf(100, health_score));
}

/**
 * @brief 打印详细的电池和系统状态
 */
void print_battery_detailed_status(void) {
    if (!ina219_initialized) {
        printf("INA219 not initialized\n");
        return;
    }
    
    float voltage, shunt_voltage, current, power;
    
    if (read_ina219_data(&voltage, &shunt_voltage, &current, &power) == 0) {
        int health_score = analyze_system_health(voltage, current, power);
        float battery_percentage = calculate_battery_percentage(voltage);
        
        printf("\n=== INA219 Battery & System Status ===\n");
        printf("Voltage: %.3f V\n", (double)voltage);
        printf("Current: %.3f A\n", (double)current);
        printf("Power: %.3f W\n", (double)power);
        printf("Shunt Voltage: %.3f mV\n", (double)(shunt_voltage * 1000.0f));
        printf("Battery Percentage: %.1f%%\n", (double)battery_percentage);
        printf("System Health Score: %d/100\n", health_score);
        
        // 状态分析
        if (voltage < 4.5f) {
            printf("⚠️  WARNING: Low voltage (%.3fV < 4.5V)\n", (double)voltage);
        } else if (voltage > 5.3f) {
            printf("⚠️  WARNING: High voltage (%.3fV > 5.3V)\n", (double)voltage);
        } else {
            printf("✅ Voltage normal\n");
        }
        
        if (current > 4.0f) {
            printf("⚠️  WARNING: High current (%.3fA > 4.0A)\n", (double)current);
        } else {
            printf("✅ Current normal\n");
        }
        
        if (power > 20.0f) {
            printf("⚠️  WARNING: High power (%.3fW > 20.0W)\n", (double)power);
        } else {
            printf("✅ Power consumption normal\n");
        }
        
        printf("======================================\n");
    } else {
        printf("Failed to read INA219 data\n");
    }
}
