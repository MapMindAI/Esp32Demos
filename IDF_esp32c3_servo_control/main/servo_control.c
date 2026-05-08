#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "driver/ledc.h"
#include "esp_log.h"
#include "app_config.h"
#include "servo_control.h"

#define TAG "SERVO_CTRL"

// Servo command types from sender
#define CMD_SERVO_DOWN 12
#define CMD_SERVO_LEFT 14
#define CMD_SERVO_STOP 15
#define CMD_SERVO_RIGHT 16
#define CMD_SERVO_UP 18

#define SERVO_MIN_ANGLE 0
#define SERVO_MAX_ANGLE 180
#define SERVO_INIT_ANGLE 90
#define SERVO_MIN_PULSE_US 500
#define SERVO_MAX_PULSE_US 2500
#define SERVO_PWM_FREQ_HZ 50
#define SERVO_PWM_RES LEDC_TIMER_14_BIT

typedef struct {
  int lr_angle;
  int ud_angle;
  bool initialized;
  bool output_enabled;
} servo_state_t;

static servo_state_t s_servo = {
    .lr_angle = SERVO_INIT_ANGLE,
    .ud_angle = SERVO_INIT_ANGLE,
    .initialized = false,
    .output_enabled = false,
};

static inline int signed_step(int step, bool invert) {
  return invert ? -step : step;
}

static uint32_t angle_to_duty(int angle) {
  if (angle < SERVO_MIN_ANGLE) {
    angle = SERVO_MIN_ANGLE;
  } else if (angle > SERVO_MAX_ANGLE) {
    angle = SERVO_MAX_ANGLE;
  }
  const uint32_t max_duty = (1U << SERVO_PWM_RES) - 1U;
  const uint32_t pulse_us = SERVO_MIN_PULSE_US +
                            (uint32_t)(angle - SERVO_MIN_ANGLE) *
                                (SERVO_MAX_PULSE_US - SERVO_MIN_PULSE_US) /
                                (SERVO_MAX_ANGLE - SERVO_MIN_ANGLE);
  const uint32_t duty = (pulse_us * SERVO_PWM_FREQ_HZ * (max_duty + 1U)) / 1000000U;
  return duty > max_duty ? max_duty : duty;
}

static void set_servo_angle(ledc_channel_t channel, int angle) {
  ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE, channel, angle_to_duty(angle)));
  ESP_ERROR_CHECK(ledc_update_duty(LEDC_LOW_SPEED_MODE, channel));
}

static void set_servo_output_enabled(bool enabled) {
  if (enabled == s_servo.output_enabled) {
    return;
  }
  if (enabled) {
    set_servo_angle(LEDC_CHANNEL_0, s_servo.lr_angle);
    set_servo_angle(LEDC_CHANNEL_1, s_servo.ud_angle);
    s_servo.output_enabled = true;
    ESP_LOGI(TAG, "Servo PWM output enabled");
    return;
  }
  ESP_ERROR_CHECK(ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0));
  ESP_ERROR_CHECK(ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, 0));
  s_servo.output_enabled = false;
  ESP_LOGI(TAG, "Servo PWM output disabled for idle power save");
}

static int decode_step(const uint8_t* data, int dlc) {
  if (dlc < 5 || data == NULL) {
    return 3;
  }
  uint32_t value = 0;
  memcpy(&value, data + 1, sizeof(value));
  if (value == 0) {
    return 3;
  }
  if (value > 20) {
    return 20;
  }
  return (int)value;
}

void servo_control_init(gpio_num_t pin_left_right, gpio_num_t pin_up_down) {
  const ledc_timer_config_t timer_cfg = {
      .speed_mode = LEDC_LOW_SPEED_MODE,
      .duty_resolution = SERVO_PWM_RES,
      .timer_num = LEDC_TIMER_0,
      .freq_hz = SERVO_PWM_FREQ_HZ,
      .clk_cfg = LEDC_AUTO_CLK,
  };
  ESP_ERROR_CHECK(ledc_timer_config(&timer_cfg));

  const ledc_channel_config_t ch_lr = {
      .gpio_num = pin_left_right,
      .speed_mode = LEDC_LOW_SPEED_MODE,
      .channel = LEDC_CHANNEL_0,
      .intr_type = LEDC_INTR_DISABLE,
      .timer_sel = LEDC_TIMER_0,
      .duty = 0,
      .hpoint = 0,
  };
  const ledc_channel_config_t ch_ud = {
      .gpio_num = pin_up_down,
      .speed_mode = LEDC_LOW_SPEED_MODE,
      .channel = LEDC_CHANNEL_1,
      .intr_type = LEDC_INTR_DISABLE,
      .timer_sel = LEDC_TIMER_0,
      .duty = 0,
      .hpoint = 0,
  };
  ESP_ERROR_CHECK(ledc_channel_config(&ch_lr));
  ESP_ERROR_CHECK(ledc_channel_config(&ch_ud));

  set_servo_output_enabled(true);
#if SERVO_POWER_SAVE_WHEN_IDLE
  set_servo_output_enabled(false);
#endif
  s_servo.initialized = true;
  ESP_LOGI(TAG, "Servo PWM init: LR pin=%d, UD pin=%d, inv_lr=%d, inv_ud=%d",
           pin_left_right, pin_up_down, SERVO_LR_INVERT, SERVO_UD_INVERT);
}

void servo_control_canbus_message_handler(int dlc, const uint8_t* data) {
  if (!s_servo.initialized || dlc <= 0 || data == NULL) {
    return;
  }
  const uint8_t cmd = data[0];
  const int step = decode_step(data, dlc);
  bool changed = false;

  switch (cmd) {
    case CMD_SERVO_UP:
      set_servo_output_enabled(true);
      s_servo.ud_angle += signed_step(step, SERVO_UD_INVERT);
      changed = true;
      break;
    case CMD_SERVO_DOWN:
      set_servo_output_enabled(true);
      s_servo.ud_angle -= signed_step(step, SERVO_UD_INVERT);
      changed = true;
      break;
    case CMD_SERVO_LEFT:
      set_servo_output_enabled(true);
      s_servo.lr_angle -= signed_step(step, SERVO_LR_INVERT);
      changed = true;
      break;
    case CMD_SERVO_RIGHT:
      set_servo_output_enabled(true);
      s_servo.lr_angle += signed_step(step, SERVO_LR_INVERT);
      changed = true;
      break;
    case CMD_SERVO_STOP:
      ESP_LOGI(TAG, "SERVO_STOP");
#if SERVO_POWER_SAVE_WHEN_IDLE
      set_servo_output_enabled(false);
#endif
      return;
    default:
      return;
  }

  if (!changed) {
    return;
  }

  if (s_servo.lr_angle < SERVO_MIN_ANGLE) {
    s_servo.lr_angle = SERVO_MIN_ANGLE;
  } else if (s_servo.lr_angle > SERVO_MAX_ANGLE) {
    s_servo.lr_angle = SERVO_MAX_ANGLE;
  }
  if (s_servo.ud_angle < SERVO_MIN_ANGLE) {
    s_servo.ud_angle = SERVO_MIN_ANGLE;
  } else if (s_servo.ud_angle > SERVO_MAX_ANGLE) {
    s_servo.ud_angle = SERVO_MAX_ANGLE;
  }

  set_servo_angle(LEDC_CHANNEL_0, s_servo.lr_angle);
  set_servo_angle(LEDC_CHANNEL_1, s_servo.ud_angle);
  ESP_LOGI(TAG, "Servo updated LR=%d UD=%d (step=%d, cmd=%u, inv_lr=%d, inv_ud=%d)",
           s_servo.lr_angle, s_servo.ud_angle, step, cmd, SERVO_LR_INVERT, SERVO_UD_INVERT);
}
