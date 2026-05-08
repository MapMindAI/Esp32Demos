#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "robot_canbus.h"
#include "app_config.h"
#include "servo_control.h"

#define TAG "SERVO_CTRL"

static void configure_power_management(void) {
#if CONFIG_PM_ENABLE
  esp_pm_config_esp32c3_t pm_cfg = {
      .max_freq_mhz = 80,
      .min_freq_mhz = 10,
      .light_sleep_enable = true,
  };
  esp_err_t err = esp_pm_configure(&pm_cfg);
  if (err == ESP_OK) {
    ESP_LOGI(TAG, "Power management enabled: DFS 10-80MHz + auto light sleep");
  } else {
    ESP_LOGW(TAG, "Failed to enable power management: %s", esp_err_to_name(err));
  }
#else
  ESP_LOGW(TAG, "CONFIG_PM_ENABLE is off; enable it in sdkconfig for lower C3 power");
#endif
}

void app_main(void) {
  ESP_LOGI(TAG, "Init servo controller");
  configure_power_management();
  servo_control_init(SERVO_LEFT_RIGHT_PIN, SERVO_UP_DOWN_PIN);
  InitializeCanbus(CANBUS_RX_PIN, CANBUS_TX_PIN);
  SetCanbusMessageHandler(servo_control_canbus_message_handler);
  ESP_LOGI(TAG, "Ready: CAN RX=%d TX=%d, Servo LR=%d UD=%d",
           CANBUS_RX_PIN, CANBUS_TX_PIN, SERVO_LEFT_RIGHT_PIN, SERVO_UP_DOWN_PIN);

  while (1) {
    vTaskDelay(pdMS_TO_TICKS(10000));
  }
}
