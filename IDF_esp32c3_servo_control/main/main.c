#include <stdint.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "robot_canbus.h"


#define TAG "[SERVO Control]"


#define CANBUS_RX_PIN GPIO_NUM_5
#define CANBUS_TX_PIN GPIO_NUM_6

#define SERVO_LEFT_RIGHT_PIN GPIO_NUM_5
#define SERVO_UP_DOWN_PIN    GPIO_NUM_5

void app_main(void) {
  ESP_LOGI(TAG, "Hello from app_main!");

  InitializeCanbus(CANBUS_RX_PIN, CANBUS_TX_PIN);


  while (1) {
    ESP_LOGI(TAG, "Hello from app_main!");
    vTaskDelay(100);
  }
}
