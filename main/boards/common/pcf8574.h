// pcf8574.h
#ifndef PCF8574_H
#define PCF8574_H
// #if (ESP_IDF_VERSION_MAJOR >= 5)
#include "esp_rom_gpio.h" // Ensure the header defining esp_rom_gpio_pad_select_gpio is included
#define gpio_pad_select_gpio esp_rom_gpio_pad_select_gpio
// #endif
#include <esp_io_expander_tca9554.h>
#define PCF8574_TAG "PCF8574"
// 定义I2C地址
#define PCF8574_ADDRESS 0x27

// 定义步进电机引脚
#define MOTOR1_IN1 0
#define MOTOR1_IN2 1
#define MOTOR1_IN3 2
#define MOTOR1_IN4 3
#define MOTOR2_IN1 4
#define MOTOR2_IN2 5
#define MOTOR2_IN3 6
#define MOTOR2_IN4 7

// #define SDA_PIN 7
// #define SCL_PIN 8
// #define INT_PIN 46

#define MOTOR_ENABLE_PIN     GPIO_NUM_33    // 电机开关引脚
#include "i2c_device.h" // Ensure this header defines the I2cDevice class
#include "esp_log.h"    // Include ESP-IDF logging header for ESP_LOGI
#include "freertos/FreeRTOS.h" // Include FreeRTOS header for pdMS_TO_TICKS

class Pcf8574 : public I2cDevice
{
public:
     esp_io_expander_handle_t io_expander2_ = NULL;
     esp_io_expander_handle_t io_expander_ = NULL;
     i2c_master_bus_handle_t i2c_bus_;

    Pcf8574(i2c_master_bus_handle_t i2c_bus, uint8_t addr ,esp_io_expander_handle_t io_expander) : I2cDevice(i2c_bus, addr)
    {
        // uint8_t chip_id = ReadReg(0xA7);
        // ESP_LOGI(PCF8574_TAG, "Get chip ID: 0x%02X", chip_id);
        // read_buffer_ = new uint8_t[6];
        i2c_bus_=i2c_bus;
        io_expander_=io_expander;
        InitializeTca9554_2();
        // InitializeTca9554();
        ESP_LOGI(PCF8574_TAG, "PCF8574+tca2 initialized successfully");
    }

    ~Pcf8574()
    {
        // delete[] read_buffer_;
        ESP_LOGI(PCF8574_TAG, "PCF8574 deinitialized successfully");
    }

    void set_io_expander(esp_io_expander_handle_t io_expander)
    {
        io_expander_=io_expander;
    }

    void setbl(uint8_t level )
    {
         ESP_ERROR_CHECK(esp_io_expander_set_level(io_expander_,IO_EXPANDER_PIN_NUM_1, level));
    }

     void setled(uint8_t level )
    {
        // 先设置为输出模式
    esp_io_expander_set_dir(io_expander_, IO_EXPANDER_PIN_NUM_7, IO_EXPANDER_OUTPUT);
         ESP_ERROR_CHECK(esp_io_expander_set_level(io_expander_,IO_EXPANDER_PIN_NUM_7, level));
    }

    void control_motor(uint8_t motor, int steps, bool direction)
    {
        ESP_LOGI(PCF8574_TAG, "Control motor %d, steps %d, direction %d", motor, steps, direction);
        motor_enable();
        // void stepMotor(uint8_t in1, uint8_t in2, uint8_t in3, uint8_t in4, int steps, bool direction, uint8_t motor)
        if (motor == 0)
        {
            stepMotor(MOTOR1_IN1, MOTOR1_IN2, MOTOR1_IN3, MOTOR1_IN4, steps, direction, motor);
        }
        else
        {
            stepMotor(MOTOR2_IN1, MOTOR2_IN2, MOTOR2_IN3, MOTOR2_IN4, steps, direction, motor);
        }

        stopMotors();
        motor_disable();
    }
    void motor_reset()
    {
        // printf("control_motor reset.\r\n");
        // control_motor(0,343,0);control_motor(0,171,1);
        control_motor(1,200,0);control_motor(1,100,1);
        // printf("control_motor reset ok.\r\n");
    }

    void step_test( )
    {
        // i2c_master_init();
        ESP_LOGI(PCF8574_TAG, "I2C initialized successfully");
        // motor_en_init();
        motor_enable();

        // 示例：控制第一个步进电机旋转
        stepMotor(MOTOR1_IN1, MOTOR1_IN2, MOTOR1_IN3, MOTOR1_IN4, 50, 1, 0);
        ESP_LOGI(PCF8574_TAG, "Motor1 done");
        vTaskDelay(pdMS_TO_TICKS(200));
        // stopMotors();

        // 示例：控制第二个步进电机旋转
        stepMotor(MOTOR2_IN1, MOTOR2_IN2, MOTOR2_IN3, MOTOR2_IN4, 50, 0, 1);
        ESP_LOGI(PCF8574_TAG, "Motor2 done");
        vTaskDelay(pdMS_TO_TICKS(100));
        stopMotors();
        motor_disable();
        ESP_LOGI(PCF8574_TAG, "Motor test done");

        // vTaskDelete(NULL);
    }

private:
    uint8_t reg = 0x00;
    void InitializeTca9554() {
        esp_err_t ret = esp_io_expander_new_i2c_tca9554(i2c_bus_, ESP_IO_EXPANDER_I2C_TCA9554_ADDRESS_000, &io_expander_);
        if(ret != ESP_OK) {
            ret = esp_io_expander_new_i2c_tca9554(i2c_bus_, ESP_IO_EXPANDER_I2C_TCA9554A_ADDRESS_000, &io_expander_);
            if(ret != ESP_OK) {
                ESP_LOGE(PCF8574_TAG, "TCA9554 create returned error");  
                return;
            }
        }
        // 配置IO0-IO3为输出模式
        ESP_ERROR_CHECK(esp_io_expander_set_dir(io_expander_, 
            IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_1 | 
            IO_EXPANDER_PIN_NUM_2 | IO_EXPANDER_PIN_NUM_3  | IO_EXPANDER_PIN_NUM_7, 
            IO_EXPANDER_OUTPUT));

        // 复位LCD和TouchPad bl=1 rst=2
        // ESP_ERROR_CHECK(esp_io_expander_set_level(io_expander_,
        //     IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_1 | IO_EXPANDER_PIN_NUM_2 | IO_EXPANDER_PIN_NUM_7, 1));
        // vTaskDelay(pdMS_TO_TICKS(300));
        // ESP_ERROR_CHECK(esp_io_expander_set_level(io_expander_,
        //     IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_1 | IO_EXPANDER_PIN_NUM_2 | IO_EXPANDER_PIN_NUM_7, 0));
        // vTaskDelay(pdMS_TO_TICKS(300));
        // ESP_ERROR_CHECK(esp_io_expander_set_level(io_expander_,
        //     IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_1 | IO_EXPANDER_PIN_NUM_2 | IO_EXPANDER_PIN_NUM_7, 1));
        //     ESP_ERROR_CHECK(esp_io_expander_set_level(io_expander_,IO_EXPANDER_PIN_NUM_7, 0));

            
    }
    void InitializeTca9554_2() {
        esp_err_t ret = esp_io_expander_new_i2c_tca9554(i2c_bus_, ESP_IO_EXPANDER_I2C_TCA9554_ADDRESS_011, &io_expander2_);
        if(ret != ESP_OK) {
            ret = esp_io_expander_new_i2c_tca9554(i2c_bus_, ESP_IO_EXPANDER_I2C_TCA9554A_ADDRESS_011, &io_expander2_);
            if(ret != ESP_OK) {
                
                    ret = esp_io_expander_new_i2c_tca9554(i2c_bus_, ESP_IO_EXPANDER_I2C_TCA9554_ADDRESS_001, &io_expander2_);
                    if(ret != ESP_OK) {
                        ret = esp_io_expander_new_i2c_tca9554(i2c_bus_, ESP_IO_EXPANDER_I2C_TCA9554A_ADDRESS_001, &io_expander2_);
                        if(ret != ESP_OK) {
                        ESP_LOGE(PCF8574_TAG, "TCA9554-2 create returned error");  
                        return;
                        }
                    }
                
            }
        }
        // 配置IO
        // IO0=nc,IO1=usb-det, IO2=chrg, IO3=rtc-int, IO4=hall1, IO5=hall2, IO6=ir, IO7=motor-en
        ESP_ERROR_CHECK(esp_io_expander_set_dir(io_expander2_, IO_EXPANDER_PIN_NUM_7 ,  IO_EXPANDER_OUTPUT));
        ESP_ERROR_CHECK(esp_io_expander_set_dir(io_expander2_, 
            IO_EXPANDER_PIN_NUM_4 | IO_EXPANDER_PIN_NUM_1 | 
            IO_EXPANDER_PIN_NUM_2 | IO_EXPANDER_PIN_NUM_5 | IO_EXPANDER_PIN_NUM_3, 
            IO_EXPANDER_INPUT));

        //关闭电机
        ESP_ERROR_CHECK(esp_io_expander_set_level(io_expander2_, IO_EXPANDER_PIN_NUM_7, 0));
    }
    bool hallread(uint8_t motor)
    {
        // Wire.end();
        // Wire.begin(17, 18);
        uint8_t hall = IO_EXPANDER_PIN_NUM_5;//HALL2;
        if(motor == 1) {
            hall = IO_EXPANDER_PIN_NUM_4; // HALL1
        }
        uint32_t level_mask = 0;
        ESP_ERROR_CHECK( esp_io_expander_get_level(io_expander2_, hall, &level_mask));
        if (level_mask == 0)
        {
            // showled(1, false);
            // Serial.println();
            ESP_LOGI(PCF8574_TAG, "hallread hall%dyes.", motor);
            return true;
        }
        return false;
    }
    void motor_en_init()
    {
        // 配置电机开关引脚
        // esp_rom_gpio_pad_select_gpio(MOTOR_ENABLE_PIN);
        // gpio_set_direction(MOTOR_ENABLE_PIN, GPIO_MODE_OUTPUT);
        // gpio_set_level(MOTOR_ENABLE_PIN, 1);
    }
    void motor_enable()
    {
        // motor_en_init();
        // gpio_set_level(MOTOR_ENABLE_PIN, 1);
        ESP_ERROR_CHECK(esp_io_expander_set_level(io_expander2_, IO_EXPANDER_PIN_NUM_7, 1));
    }
    void motor_disable()
    {
        // motor_en_init();
        // gpio_set_level(MOTOR_ENABLE_PIN, 0);
        ESP_ERROR_CHECK(esp_io_expander_set_level(io_expander2_, IO_EXPANDER_PIN_NUM_7, 0));
    }
    void stopMotors()
    {
        // 初始化步进电机引脚为输出
        for (int i = 0; i < 8; i++)
        {
            pcf8574_write(0x00);
        }
        //   motor_disable();
    }
    void pcf8574_write(uint8_t data)
    {
        WriteReg(reg, data);
    }

    void stepMotor(uint8_t in1, uint8_t in2, uint8_t in3, uint8_t in4, int steps, bool direction, uint8_t motor)
    {
        uint8_t stepsForward[4][4] = {
            {1, 0, 0, 0},
            {0, 1, 0, 0},
            {0, 0, 1, 0},
            {0, 0, 0, 1}};

        uint8_t stepsReverse[4][4] = {
            {0, 0, 0, 1},
            {0, 0, 1, 0},
            {0, 1, 0, 0},
            {1, 0, 0, 0}};

        uint8_t(*stepsArray)[4] = (direction == false) ? stepsForward : stepsReverse;

        for (int i = 0; i < steps; i++)
        {
            if (direction == false && hallread(motor))
            break;
            for (int j = 0; j < 4; j++)
            {
                uint8_t data = (stepsArray[j][0] << in1) | (stepsArray[j][1] << in2) | (stepsArray[j][2] << in3) | (stepsArray[j][3] << in4);
                pcf8574_write(data);
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        }
    }
    // uint8_t *read_buffer_ = nullptr;
    // TouchPoint_t tp_;
};


class Cst816x : public I2cDevice
{
public:
    struct TouchPoint_t
    {
        int num = 0;
        int x = -1;
        int y = -1;
    };

    Cst816x(i2c_master_bus_handle_t i2c_bus, uint8_t addr) : I2cDevice(i2c_bus, addr)
    {
        uint8_t chip_id = ReadReg(0xA7);
        ESP_LOGI(PCF8574_TAG, "Get chip ID: 0x%02X", chip_id);
        read_buffer_ = new uint8_t[6];
    }

    ~Cst816x()
    {
        delete[] read_buffer_;
    }

    void UpdateTouchPoint()
    {
        ReadRegs(0x02, read_buffer_, 6);
        tp_.num = read_buffer_[0] & 0x0F;
        tp_.x = ((read_buffer_[1] & 0x0F) << 8) | read_buffer_[2];
        tp_.y = ((read_buffer_[3] & 0x0F) << 8) | read_buffer_[4];
    }

    const TouchPoint_t &GetTouchPoint()
    {
        return tp_;
    }

private:
    uint8_t *read_buffer_ = nullptr;
    TouchPoint_t tp_;
};

#endif // PCF8574_H