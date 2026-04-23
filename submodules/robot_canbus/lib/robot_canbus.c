

#include "robot_canbus.h"
#include "driver/twai.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define TAG "[CANBUS]"

CanbusMessageHandler canbus_message_handler_ = NULL;
void SetCanbusMessageHandler(CanbusMessageHandler fcn) { canbus_message_handler_ = fcn; }

static void canbus_receive_task(void* param) {
  // 4. 创建接收任务
  twai_message_t rx_msg;
  while (1) {
    if (twai_receive(&rx_msg, pdMS_TO_TICKS(1000)) == ESP_OK) {
      ESP_LOGI(TAG, "Received CAN ID: 0x%03X, DLC: %d, Data: %02X %02X %02X %02X %02X %02X %02X %02X",
               rx_msg.identifier, rx_msg.data_length_code, rx_msg.data[0], rx_msg.data[1], rx_msg.data[2],
               rx_msg.data[3], rx_msg.data[4], rx_msg.data[5], rx_msg.data[6], rx_msg.data[7]);
      if (rx_msg.identifier == CANBUS_MESSAGE_ID && canbus_message_handler_) {
        canbus_message_handler_(rx_msg.data_length_code, rx_msg.data);
      }
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  vTaskDelete(NULL);
}

#define MAX_QUEUE_ELEMENT 10

QueueHandle_t canbus_send_queue_;

void AddMessageToSend(uint8_t message_type, uint32_t value) {
  twai_message_t msg = {.identifier = CANBUS_MESSAGE_ID, .data_length_code = 5, .flags = TWAI_MSG_FLAG_NONE};
  msg.data[0] = message_type;
  memcpy(msg.data + 1, &value, 4);
  xQueueSend(canbus_send_queue_, &msg, pdMS_TO_TICKS(10));
}

static void canbus_send_task(void* param) {
  if (!canbus_send_queue_) {
    ESP_LOGE(TAG, "send queue empty.");
    vTaskDelete(NULL);
    return;
  }

  twai_message_t tx_msg_beats = {
      .identifier = CANBUS_MESSAGE_ID, .data_length_code = 2, .data = {0xAB, 0xCD}, .flags = TWAI_MSG_FLAG_NONE};

  twai_message_t tx_msg;
  while (1) {
    if (xQueueReceive(canbus_send_queue_, &tx_msg, pdMS_TO_TICKS(1000))) {
      twai_transmit(&tx_msg, pdMS_TO_TICKS(CANBUS_MAX_WAIT_MS));
    } else {
      // send heart beat
      twai_transmit(&tx_msg_beats, pdMS_TO_TICKS(CANBUS_MAX_WAIT_MS));
    }
  }
  vTaskDelete(NULL);
}

void InitializeCanbus(gpio_num_t pin_rx, gpio_num_t pin_tx) {
  // 1. 配置 TWAI
  twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT(pin_tx, pin_rx, TWAI_MODE_NORMAL);
  twai_timing_config_t t_config = TWAI_TIMING_CONFIG_500KBITS();  // 500 kbps
  twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

  // 2. 安装驱动
  if (twai_driver_install(&g_config, &t_config, &f_config) != ESP_OK) {
    ESP_LOGE(TAG, "Failed to install TWAI driver");
    return;
  }

  // 3. 启动驱动
  if (twai_start() != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start TWAI driver");
    return;
  }

  // initialize queue for canbus message
  canbus_send_queue_ = xQueueCreate(MAX_QUEUE_ELEMENT, sizeof(twai_message_t));

  ESP_LOGI(TAG, "TWAI CAN initialized!");

  xTaskCreatePinnedToCore(canbus_receive_task, "canbus_receive_task", 4096, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(canbus_send_task, "canbus_send_task", 4096, NULL, 1, NULL, 0);
}
