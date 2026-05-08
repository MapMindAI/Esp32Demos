#pragma once

#include <stdint.h>
#include "driver/gpio.h"

void servo_control_init(gpio_num_t pin_left_right, gpio_num_t pin_up_down);
void servo_control_canbus_message_handler(int dlc, const uint8_t* data);
