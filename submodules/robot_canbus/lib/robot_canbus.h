#pragma once

#include "esp_task_wdt.h"
#include "driver/gpio.h"

#define CANBUS_MAX_WAIT_MS 100
#define CANBUS_MESSAGE_ID  0x1F

typedef void (*CanbusMessageHandler)(int, const uint8_t*);
void SetCanbusMessageHandler(CanbusMessageHandler fcn);



void InitializeCanbus(gpio_num_t pin_rx, gpio_num_t pin_tx);
void AddMessageToSend(uint8_t message_type, uint32_t value);
