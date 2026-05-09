#include <stdint.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "ble/ble_client.h"
#include "config.h"
#include "control/control_callback.h"

#include "esp_intr_alloc.h"

#include "esp_log.h"
#include "esp_task_wdt.h"
#include "robot_canbus.h"

#define TAG "[MKBOT]"

#define CANBUS_RX_PIN  GPIO_NUM_26
#define CANBUS_TX_PIN  GPIO_NUM_25

void app_main(void) {
  ESP_LOGI(TAG, "Hello from MOKUKU ROBOT %s!", MOKUKU_VERSION);
  SetUpLed();
  InitMotorMcp23017();

  SetCanbusMessageHandler(CansbusControlMessageCallback);
  InitializeCanbus(CANBUS_RX_PIN, CANBUS_TX_PIN);

  SetBleMessageHandler(ControlMessageCallback);
  ObdBleClientSetup();

  while (1) {
    int64_t boottime_ms = esp_timer_get_time();
    UpdateLed(boottime_ms);
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}
