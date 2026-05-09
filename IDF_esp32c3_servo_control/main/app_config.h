#pragma once

#include "driver/gpio.h"

// CAN bus interface pins
#define CANBUS_RX_PIN GPIO_NUM_5
#define CANBUS_TX_PIN GPIO_NUM_6

// Servo PWM output pins (must not overlap CAN pins)
#define SERVO_LEFT_RIGHT_PIN GPIO_NUM_4
#define SERVO_UP_DOWN_PIN GPIO_NUM_3

// Inversion flags for installation direction (0: normal, 1: inverted)
#define SERVO_LR_INVERT 1
#define SERVO_UD_INVERT 1

// Power saving: disable PWM output on SERVO_STOP to reduce holding current.
// Tradeoff: servo will not hold torque while idle.
#define SERVO_POWER_SAVE_WHEN_IDLE 0
