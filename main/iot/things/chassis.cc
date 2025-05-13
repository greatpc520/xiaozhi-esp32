/*
    ESP-SparkBot 的底座
    https://gitee.com/esp-friends/esp_sparkbot/tree/master/example/tank/c2_tracked_chassis
*/

#include "sdkconfig.h"
#include "iot/thing.h"
#include "board.h"

#include <driver/gpio.h>
#include <driver/uart.h>
#include <esp_log.h>
#include <cstring>

// #include "boards/esp32s3-korvo2-v3/config.h"
typedef enum {
    LIGHT_MODE_CHARGING_BREATH = 0,
    LIGHT_MODE_POWER_LOW,
    LIGHT_MODE_ALWAYS_ON,
    LIGHT_MODE_BLINK,
    LIGHT_MODE_WHITE_BREATH_SLOW,
    LIGHT_MODE_WHITE_BREATH_FAST,
    LIGHT_MODE_FLOWING,
    LIGHT_MODE_SHOW,
    LIGHT_MODE_SLEEP,
    LIGHT_MODE_MAX
} light_mode_t;

#define TAG "Chassis"

namespace iot {

class Chassis : public Thing {
private:
    light_mode_t light_mode_ = LIGHT_MODE_ALWAYS_ON;
    void set_backlight(uint8_t brightness)
    {
        auto& board = Board::GetInstance();
        // board.GetBacklight()->SetBrightness(brightness);
        auto motor = board.SetMotor();
        uint8_t level=0;
        if(brightness>0)level=1;

        motor->setbl(level);
    }
        void set_led(uint8_t brightness)
    {
        auto& board = Board::GetInstance();
        // board.GetBacklight()->SetBrightness(brightness);
        auto motor = board.SetMotor();
        uint8_t level=0;
        if(brightness>0)level=1;

        motor->setled(level);
    }
    void control_motor(uint8_t motorid, int steps, bool direction)
    {
        // return;
        // static bool isup=true;
        // if(direction && isup) return;
        // if(direction==false && isup==false) return;
        // isup=direction;
        auto& board = Board::GetInstance();
        auto motor = board.SetMotor();
        // motor->setbl((uint8_t)direction);
        motor->control_motor(motorid, steps, direction);
    
    }

/*    
void SendUartMessage(const char * command_str) {
        uint8_t len = strlen(command_str);
        uart_write_bytes(ECHO_UART_PORT_NUM, command_str, len);
        ESP_LOGI(TAG, "Sent command: %s", command_str);
    }
    void InitializeEchoUart() {
        uart_config_t uart_config = {
            .baud_rate = ECHO_UART_BAUD_RATE,
            .data_bits = UART_DATA_8_BITS,
            .parity    = UART_PARITY_DISABLE,
            .stop_bits = UART_STOP_BITS_1,
            .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
            .source_clk = UART_SCLK_DEFAULT,
        };
        int intr_alloc_flags = 0;

        ESP_ERROR_CHECK(uart_driver_install(ECHO_UART_PORT_NUM, BUF_SIZE * 2, 0, 0, NULL, intr_alloc_flags));
        ESP_ERROR_CHECK(uart_param_config(ECHO_UART_PORT_NUM, &uart_config));
        ESP_ERROR_CHECK(uart_set_pin(ECHO_UART_PORT_NUM, UART_ECHO_TXD, UART_ECHO_RXD, UART_ECHO_RTS, UART_ECHO_CTS));

        SendUartMessage("w2");
    }
*/

public:
    Chassis() : Thing("Chassis", "终端：有云台可以上下左右旋转；可以开关屏幕；开关指示灯；可以重复操作"), light_mode_(LIGHT_MODE_ALWAYS_ON) {
        // InitializeEchoUart();

        // 定义设备的属性
        // properties_.AddNumberProperty("light_mode", "灯光效果编号", [this]() -> int {
        //     return (light_mode_ - 2 <= 0) ? 1 : light_mode_ - 2;
        // });

        // 定义设备可以被远程执行的指令

        

        methods_.AddMethod("reset", "复位终端", ParameterList(), [this](const ParameterList& parameters) {
            printf("control_motor reset.\r\n");
        control_motor(0,343,0);control_motor(0,171,1);
        control_motor(1,200,0);control_motor(1,100,1);
        printf("control_motor reset ok.\r\n");
        });
        methods_.AddMethod("GoForward", "向上转", ParameterList(), [this](const ParameterList& parameters) {
            // SendUartMessage("x0.0 y1.0");
            control_motor(1, 30, true);
        });

        methods_.AddMethod("GoBack", "向下转", ParameterList(), [this](const ParameterList& parameters) {
            // SendUartMessage("x0.0 y-1.0");
            control_motor(1, 30, false);
        });

        methods_.AddMethod("TurnLeft", "向左转", ParameterList(), [this](const ParameterList& parameters) {
            // SendUartMessage("x-1.0 y0.0");
            control_motor(0, 30, false);
        });

        methods_.AddMethod("TurnRight", "向右转", ParameterList(), [this](const ParameterList& parameters) {
            // SendUartMessage("x1.0 y0.0");
            control_motor(0, 30, true);
        });

        methods_.AddMethod("BlOff", "打开屏幕", ParameterList(), [this](const ParameterList& parameters) {
            // SendUartMessage("x1.0 y0.0");
            set_backlight(0);
        });

        methods_.AddMethod("BlOn", "关闭屏幕", ParameterList(), [this](const ParameterList& parameters) {
            // SendUartMessage("x1.0 y0.0");
            set_backlight(1);
        });

        
        methods_.AddMethod("TurnOn", "打开灯", ParameterList(), [this](const ParameterList& parameters) {
            set_led(0);
        });

        methods_.AddMethod("TurnOff", "关闭灯", ParameterList(), [this](const ParameterList& parameters) {
                        set_led(1);
        });
/*
        methods_.AddMethod("Dance", "跳舞", ParameterList(), [this](const ParameterList& parameters) {
            SendUartMessage("d1");
            light_mode_ = LIGHT_MODE_MAX;
        });

        methods_.AddMethod("SwitchLightMode", "打开灯", ParameterList({
            Parameter("lightmode", "1到6之间的整数", kValueTypeNumber, true)
        }), [this](const ParameterList& parameters) {
            char command_str[5] = {'w', 0, 0};
            char mode = static_cast<char>(parameters["lightmode"].number()) + 2;

            ESP_LOGI(TAG, "Input Light Mode: %c", (mode + '0'));

            if (mode >= 3 && mode <= 8) {
                command_str[1] = mode + '0';
                SendUartMessage(command_str);
            }
        });
        */
    }
};

} // namespace iot

DECLARE_THING(Chassis);
