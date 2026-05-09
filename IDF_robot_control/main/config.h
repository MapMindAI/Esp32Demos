#pragma once

#include <stdlib.h>
#include <string.h>
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
