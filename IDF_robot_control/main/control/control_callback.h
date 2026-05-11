#pragma once

#include "config.h"
#include "esp_task_wdt.h"

#define MOTOR_STBY_MCP_PIN   0

#define MOTOR_A_BIN1_MCP_PIN 1
#define MOTOR_A_BIN2_MCP_PIN 2
#define MOTOR_A_PWMA_PIN GPIO_NUM_16
#define MOTOR_A_PWM_CHANNEL LEDC_CHANNEL_0

#define MOTOR_B_BIN1_MCP_PIN 3
#define MOTOR_B_BIN2_MCP_PIN 4
#define MOTOR_B_PWMA_PIN GPIO_NUM_17
#define MOTOR_B_PWM_CHANNEL LEDC_CHANNEL_1

#define MOTOR_C_BIN1_MCP_PIN 5
#define MOTOR_C_BIN2_MCP_PIN 6
#define MOTOR_C_PWMA_PIN GPIO_NUM_18
#define MOTOR_C_PWM_CHANNEL LEDC_CHANNEL_2

#define MOTOR_D_BIN1_MCP_PIN 7
#define MOTOR_D_BIN2_MCP_PIN 8
#define MOTOR_D_PWMA_PIN GPIO_NUM_19
#define MOTOR_D_PWM_CHANNEL LEDC_CHANNEL_3

#define I2C_MASTER_NUM              0
#define  MCP23017_I2C_ADDR        0x27
#define  MCP23017_I2C_MASTER_SDA  GPIO_NUM_21
#define  MCP23017_I2C_MASTER_SCL  GPIO_NUM_22


// if no message for a long time, we will stop the robot
#define NO_MESSAGE_STOP_DELAY 1000000

void InitMotorMcp23017();

void SetUpLed();
void UpdateLed(int64_t boottime_ms);

// param->write.value, param->write.len
void ControlMessageCallback(int len, const uint8_t* value);
void CansbusControlMessageCallback(int len, const uint8_t* value);
