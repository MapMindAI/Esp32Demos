#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "driver/ledc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
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
#define SERVO_POWER_SAVE_CHECK_PERIOD_MS 200

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
static SemaphoreHandle_t s_servo_lock = NULL;
static TaskHandle_t s_power_save_task = NULL;
static int64_t s_last_activity_us = 0;

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

static void note_servo_activity_locked(void) {
  s_last_activity_us = esp_timer_get_time();
}

#if SERVO_POWER_SAVE_WHEN_IDLE
static void servo_idle_power_save_task(void* arg) {
  while (1) {
    if (s_servo.initialized && s_servo.output_enabled) {
      xSemaphoreTake(s_servo_lock, portMAX_DELAY);
      const int64_t now = esp_timer_get_time();
      const int64_t idle_us = now - s_last_activity_us;
      if (idle_us >= (int64_t)SERVO_IDLE_POWER_SAVE_TIMEOUT_MS * 1000LL) {
        set_servo_output_enabled(false);
        ESP_LOGI(TAG, "Servo auto power-save after %d ms idle", SERVO_IDLE_POWER_SAVE_TIMEOUT_MS);
      }
      xSemaphoreGive(s_servo_lock);
    }
    vTaskDelay(pdMS_TO_TICKS(SERVO_POWER_SAVE_CHECK_PERIOD_MS));
  }
}
#endif

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
  if (s_servo_lock == NULL) {
    s_servo_lock = xSemaphoreCreateMutex();
    if (s_servo_lock == NULL) {
      ESP_LOGE(TAG, "Failed to create servo lock");
      abort();
    }
  }

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

  xSemaphoreTake(s_servo_lock, portMAX_DELAY);
  set_servo_output_enabled(true);
#if SERVO_POWER_SAVE_WHEN_IDLE
  note_servo_activity_locked();
#endif
  s_servo.initialized = true;
  xSemaphoreGive(s_servo_lock);

#if SERVO_POWER_SAVE_WHEN_IDLE
  if (s_power_save_task == NULL) {
    xTaskCreatePinnedToCore(servo_idle_power_save_task, "servo_idle_ps", 3072, NULL, 2, &s_power_save_task, 0);
  }
#endif
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

  xSemaphoreTake(s_servo_lock, portMAX_DELAY);

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
      // Keep output enabled briefly; idle task will power-save after timeout.
      set_servo_angle(LEDC_CHANNEL_0, s_servo.lr_angle);
      set_servo_angle(LEDC_CHANNEL_1, s_servo.ud_angle);
      xSemaphoreGive(s_servo_lock);
      return;
    default:
      xSemaphoreGive(s_servo_lock);
      return;
  }

  if (!changed) {
    xSemaphoreGive(s_servo_lock);
    return;
  }

  note_servo_activity_locked();
  
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
  xSemaphoreGive(s_servo_lock);
}
