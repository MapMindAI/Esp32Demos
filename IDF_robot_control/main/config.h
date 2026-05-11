#pragma once

#include <stdlib.h>
#include <string.h>
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"

#define MOKUKU_VERSION "v0.0.1"

#define DEBUG_MODE

#if defined(DEBUG_MODE)
#define DMLOG(...) printf(__VA_ARGS__)
#else
#define DMLOG(...) ((void)0)  // do nothing
#endif


#if CONFIG_IDF_TARGET_ESP32C3
// ESP32-C3 specific
// https://github.com/sidharthmohannair/Tutorial-ESP32-C3-Super-Mini/blob/main/docs/examples/Blink/README.md
#define LED_PIN GPIO_NUM_8
// turn the LED on (LOW because the LED is inverted)
#define LED_LIGHT_OFF 1
#define LED_LIGHT_ON 0
#else
// Regular ESP32 (e.g. DevKit V1)
#define LED_PIN GPIO_NUM_2
#define LED_LIGHT_OFF 0
#define LED_LIGHT_ON 1
#endif   // #if CONFIG_IDF_TARGET_ESP32C3


// MCP2515 PINS
#define PIN_NUM_MISO GPIO_NUM_25
#define PIN_NUM_MOSI GPIO_NUM_26
#define PIN_NUM_CLK  GPIO_NUM_32
#define PIN_NUM_CS   GPIO_NUM_33
// Set to GPIO number if MCP2515 RESET is wired; keep -1 if not connected.
#define MCP2515_RESET_GPIO (-1)

// MCP2515 bus settings
#define MCP2515_XTAL_MHZ 8
#define MCP2515_CAN_SPEED_KBPS 500

// MCP2515 SPI settings (this does NOT change CAN bus bitrate)
#define MCP2515_SPI_CLOCK_HZ 1000000
#define MCP2515_DEBUG_ACCEPT_ALL 1
