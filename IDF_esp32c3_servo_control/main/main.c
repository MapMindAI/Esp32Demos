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


#define TAG "[SERVO Control]"


void app_main(void) {
  ESP_LOGI(TAG, "Hello from app_main!");

  while (1) {
    ESP_LOGI(TAG, "Hello from app_main!");
    vTaskDelay(100);
  }
}
