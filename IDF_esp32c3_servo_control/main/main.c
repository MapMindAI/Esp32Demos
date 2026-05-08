#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "robot_canbus.h"
#include "app_config.h"
#include "servo_control.h"

#define TAG "SERVO_CTRL"

void app_main(void) {
  ESP_LOGI(TAG, "Init servo controller");
  servo_control_init(SERVO_LEFT_RIGHT_PIN, SERVO_UP_DOWN_PIN);
  InitializeCanbus(CANBUS_RX_PIN, CANBUS_TX_PIN);
  SetCanbusMessageHandler(servo_control_canbus_message_handler);
  ESP_LOGI(TAG, "Ready: CAN RX=%d TX=%d, Servo LR=%d UD=%d",
           CANBUS_RX_PIN, CANBUS_TX_PIN, SERVO_LEFT_RIGHT_PIN, SERVO_UP_DOWN_PIN);

  while (1) {
    vTaskDelay(pdMS_TO_TICKS(2000));
  }
}
