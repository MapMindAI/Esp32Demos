

#include "control/control_callback.h"
// #include "driver/adc.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <mcp23x17.h>


#define TAG "[CONTORL]"
#define DEFAULT_MOTOR_SPEED 125
#define MOTOR_CONTROL_PERIOD_MS 20
#define MOTOR_ACCEL_STEP 8
#define MOTOR_DECEL_STEP 12

// static EventGroupHandle_t event_grounp_handle_ = NULL;
static mcp23x17_t mcp23x17_device_ = { 0 };
static SemaphoreHandle_t motor_lock_ = NULL;
static TaskHandle_t motor_control_task_handle_ = NULL;

typedef enum {
  MOTION_STOP = 0,
  MOTION_FORWARD,
  MOTION_BACKWARD,
  MOTION_LEFT,
  MOTION_RIGHT,
  MOTION_ROTATE_LEFT,
  MOTION_ROTATE_RIGHT,
} motion_cmd_t;

static motion_cmd_t requested_motion_ = MOTION_STOP;
static motion_cmd_t current_motion_ = MOTION_STOP;
static int requested_speed_ = 0;
static int current_speed_ = 0;
static int configured_speed_ = DEFAULT_MOTOR_SPEED;

static inline void motor_lock(void) {
  if (motor_lock_) {
    xSemaphoreTake(motor_lock_, portMAX_DELAY);
  }
}

static inline void motor_unlock(void) {
  if (motor_lock_) {
    xSemaphoreGive(motor_lock_);
  }
}

static inline void mcp_set_level_checked(uint8_t pin, uint32_t level) {
  esp_err_t err = mcp23x17_set_level(&mcp23x17_device_, pin, level);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "mcp23x17_set_level(pin=%u, level=%u) failed: %s", pin, level, esp_err_to_name(err));
  }
}

static inline void set_motor_standby(bool enable) {
  mcp_set_level_checked(MOTOR_STBY_MCP_PIN, enable ? 1 : 0);
}

static void motor_pwm_init() {
  ledc_timer_config_t timer = {.speed_mode = LEDC_LOW_SPEED_MODE,
                               .timer_num = LEDC_TIMER_0,
                               .duty_resolution = LEDC_TIMER_8_BIT,
                               .freq_hz = 1000,
                               .clk_cfg = LEDC_AUTO_CLK};
  ledc_timer_config(&timer);
  ledc_channel_config_t chA = {.gpio_num = MOTOR_A_PWMA_PIN,
                               .speed_mode = LEDC_LOW_SPEED_MODE,
                               .channel = MOTOR_A_PWM_CHANNEL,
                               .timer_sel = LEDC_TIMER_0,
                               .duty = 0};
  ledc_channel_config(&chA);
  ledc_channel_config_t chB = {.gpio_num = MOTOR_B_PWMA_PIN,
                               .speed_mode = LEDC_LOW_SPEED_MODE,
                               .channel = MOTOR_B_PWM_CHANNEL,
                               .timer_sel = LEDC_TIMER_0,
                               .duty = 0};
  ledc_channel_config(&chB);
  ledc_channel_config_t chC = {.gpio_num = MOTOR_C_PWMA_PIN,
                               .speed_mode = LEDC_LOW_SPEED_MODE,
                               .channel = MOTOR_C_PWM_CHANNEL,
                               .timer_sel = LEDC_TIMER_0,
                               .duty = 0};
  ledc_channel_config(&chC);
  ledc_channel_config_t chD = {.gpio_num = MOTOR_D_PWMA_PIN,
                               .speed_mode = LEDC_LOW_SPEED_MODE,
                               .channel = MOTOR_D_PWM_CHANNEL,
                               .timer_sel = LEDC_TIMER_0,
                               .duty = 0};
  ledc_channel_config(&chD);
}

static void motor_set_speed(int speedA, int speedB, int speedC, int speedD) {
  ledc_set_duty(LEDC_LOW_SPEED_MODE, MOTOR_A_PWM_CHANNEL, speedA);
  ledc_update_duty(LEDC_LOW_SPEED_MODE, MOTOR_A_PWM_CHANNEL);
  ledc_set_duty(LEDC_LOW_SPEED_MODE, MOTOR_B_PWM_CHANNEL, speedB);
  ledc_update_duty(LEDC_LOW_SPEED_MODE, MOTOR_B_PWM_CHANNEL);
  ledc_set_duty(LEDC_LOW_SPEED_MODE, MOTOR_C_PWM_CHANNEL, speedC);
  ledc_update_duty(LEDC_LOW_SPEED_MODE, MOTOR_C_PWM_CHANNEL);
  ledc_set_duty(LEDC_LOW_SPEED_MODE, MOTOR_D_PWM_CHANNEL, speedD);
  ledc_update_duty(LEDC_LOW_SPEED_MODE, MOTOR_D_PWM_CHANNEL);
}

static void set_a_direction(bool front) {
  mcp_set_level_checked(MOTOR_A_BIN1_MCP_PIN, front ? 0 : 1);
  mcp_set_level_checked(MOTOR_A_BIN2_MCP_PIN, front ? 1 : 0);
}

static void set_b_direction(bool front) {
  mcp_set_level_checked(MOTOR_B_BIN1_MCP_PIN, front ? 0 : 1);
  mcp_set_level_checked(MOTOR_B_BIN2_MCP_PIN, front ? 1 : 0);
}

static void set_c_direction(bool front) {
  mcp_set_level_checked(MOTOR_C_BIN1_MCP_PIN, front ? 0 : 1);
  mcp_set_level_checked(MOTOR_C_BIN2_MCP_PIN, front ? 1 : 0);
}

static void set_d_direction(bool front) {
  mcp_set_level_checked(MOTOR_D_BIN1_MCP_PIN, front ? 0 : 1);
  mcp_set_level_checked(MOTOR_D_BIN2_MCP_PIN, front ? 1 : 0);
}

static void forward(int speed) {
  set_motor_standby(true);
  set_a_direction(1);
  set_b_direction(1);
  set_c_direction(1);
  set_d_direction(1);
  motor_set_speed(speed, speed, speed, speed);
}

static void backward(int speed) {
  set_motor_standby(true);
  set_a_direction(0);
  set_b_direction(0);
  set_c_direction(0);
  set_d_direction(0);
  motor_set_speed(speed, speed, speed, speed);
}

static void turn_left(int speed) {
  set_motor_standby(true);
  set_a_direction(1);
  set_b_direction(0);
  set_c_direction(1);
  set_d_direction(0);
  motor_set_speed(speed, speed, speed, speed);
}

static void turn_right(int speed) {
  set_motor_standby(true);
  set_a_direction(0);
  set_b_direction(1);
  set_c_direction(0);
  set_d_direction(1);
  motor_set_speed(speed, speed, speed, speed);
}

static void rotate_left(int speed) {
  // Mecanum yaw-left pattern (left pair reverse, right pair forward).
  set_motor_standby(true);
  set_a_direction(1);
  set_b_direction(0);
  set_c_direction(0);
  set_d_direction(1);
  motor_set_speed(speed, speed, speed, speed);
}

static void rotate_right(int speed) {
  // Mecanum yaw-right pattern (left pair forward, right pair reverse).
  set_motor_standby(true);
  set_a_direction(0);
  set_b_direction(1);
  set_c_direction(1);
  set_d_direction(0);
  motor_set_speed(speed, speed, speed, speed);
}

static void stop_soft() { motor_set_speed(0, 0, 0, 0); }

static void apply_motion_direction(motion_cmd_t motion) {
  switch (motion) {
    case MOTION_FORWARD:
      forward(0);
      break;
    case MOTION_BACKWARD:
      backward(0);
      break;
    case MOTION_LEFT:
      turn_left(0);
      break;
    case MOTION_RIGHT:
      turn_right(0);
      break;
    case MOTION_ROTATE_LEFT:
      rotate_left(0);
      break;
    case MOTION_ROTATE_RIGHT:
      rotate_right(0);
      break;
    case MOTION_STOP:
    default:
      break;
  }
}

static void set_motion_target_locked(motion_cmd_t motion, int speed) {
  if (speed < 0) {
    speed = 0;
  } else if (speed > 255) {
    speed = 255;
  }
  requested_motion_ = motion;
  requested_speed_ = (motion == MOTION_STOP) ? 0 : speed;
}

static void motor_control_task(void* arg) {
  while (1) {
    motor_lock();

    // Change direction only at zero speed to avoid abrupt torque reversal.
    if (current_motion_ != requested_motion_ && current_speed_ == 0) {
      current_motion_ = requested_motion_;
      if (current_motion_ != MOTION_STOP) {
        apply_motion_direction(current_motion_);
      }
    }

    int target_speed = 0;
    if (current_motion_ == requested_motion_) {
      target_speed = requested_speed_;
    }

    if (current_speed_ < target_speed) {
      current_speed_ += MOTOR_ACCEL_STEP;
      if (current_speed_ > target_speed) {
        current_speed_ = target_speed;
      }
    } else if (current_speed_ > target_speed) {
      current_speed_ -= MOTOR_DECEL_STEP;
      if (current_speed_ < target_speed) {
        current_speed_ = target_speed;
      }
    }

    if (current_speed_ <= 0) {
      current_speed_ = 0;
      stop_soft();
    } else {
      motor_set_speed(current_speed_, current_speed_, current_speed_, current_speed_);
    }

    motor_unlock();
    vTaskDelay(pdMS_TO_TICKS(MOTOR_CONTROL_PERIOD_MS));
  }
}

static int64_t led_updated_time_ = 0;

static int parse_speed_or_default(int len, const uint8_t* value) {
  if (len >= 5) {
    uint32_t v = 0;
    memcpy(&v, value + 1, sizeof(v));
    if (v > 255) {
      v = 255;
    }
    return (int)v;
  }
  return configured_speed_;
}

static void HandleControlMessage(int len, const uint8_t* value, const char* source) {
  if (len <= 0 || value == NULL) {
    ESP_LOGW(TAG, "invalid message from %s", source);
    return;
  }

  motor_lock();

  uint8_t message_type = value[0];
  const int speed = parse_speed_or_default(len, value);
  switch (message_type) {
    case 66:
      motor_unlock();
      esp_restart();  // Reboots the ESP32
      return;
    case 8: {
      ESP_LOGI(TAG, "%s move front speed=%d", source, speed);
      set_motion_target_locked(MOTION_FORWARD, speed);
    } break;
    case 2: {
      ESP_LOGI(TAG, "%s move back speed=%d", source, speed);
      set_motion_target_locked(MOTION_BACKWARD, speed);
    } break;
    case 4: {
      ESP_LOGI(TAG, "%s strafe left speed=%d", source, speed);
      set_motion_target_locked(MOTION_LEFT, speed);
    } break;
    case 5: {
      ESP_LOGI(TAG, "%s stop", source);
      set_motion_target_locked(MOTION_STOP, 0);
    } break;
    case 6: {
      ESP_LOGI(TAG, "%s strafe right speed=%d", source, speed);
      set_motion_target_locked(MOTION_RIGHT, speed);
    } break;
    case 7: {
      ESP_LOGI(TAG, "%s rotate left speed=%d", source, speed);
      set_motion_target_locked(MOTION_ROTATE_LEFT, speed);
    } break;
    case 9: {
      ESP_LOGI(TAG, "%s rotate right speed=%d", source, speed);
      set_motion_target_locked(MOTION_ROTATE_RIGHT, speed);
    } break;
    case 10: {
      configured_speed_ = speed;
      // Apply speed updates immediately when robot is already moving.
      if (requested_motion_ != MOTION_STOP) {
        requested_speed_ = configured_speed_;
      }
      ESP_LOGI(TAG, "%s speed update=%d", source, configured_speed_);
    } break;
    default: {
      ESP_LOG_BUFFER_HEX(TAG, value, len);
    } break;
  }
  motor_unlock();

  led_updated_time_ = esp_timer_get_time();
  gpio_set_level(LED_PIN, LED_LIGHT_ON);
}

void ControlMessageCallback(int len, const uint8_t* value) { HandleControlMessage(len, value, "BLE"); }
void CansbusControlMessageCallback(int len, const uint8_t* value) { HandleControlMessage(len, value, "CAN"); }

// LED CONTROL
void SetUpLed() {
  gpio_config_t io_conf = {
      .intr_type = GPIO_INTR_DISABLE,  // disable interrupt
      .mode = GPIO_MODE_OUTPUT,        // set as output mode
      .pin_bit_mask = 1ULL << LED_PIN,
      .pull_down_en = 0,  // disable pull-down mode
      .pull_up_en = 0,    // disable pull-up mode
  };
  // configure GPIO with the given settings
  gpio_config(&io_conf);
  gpio_set_level(LED_PIN, LED_LIGHT_OFF);
}


void InitMotorMcp23017() {
  if (motor_lock_ == NULL) {
    motor_lock_ = xSemaphoreCreateMutex();
    if (motor_lock_ == NULL) {
      ESP_LOGE(TAG, "failed to create motor mutex");
      abort();
    }
  }

  ESP_ERROR_CHECK(i2cdev_init());

  // event_grounp_handle_ = xEventGroupCreate();
  ESP_ERROR_CHECK(mcp23x17_init_desc(&mcp23x17_device_, MCP23017_I2C_ADDR, 0, MCP23017_I2C_MASTER_SDA, MCP23017_I2C_MASTER_SCL));

  // Setup ports as output
  mcp23x17_set_mode(&mcp23x17_device_, MOTOR_STBY_MCP_PIN, MCP23X17_GPIO_OUTPUT);
  mcp23x17_set_mode(&mcp23x17_device_, MOTOR_A_BIN1_MCP_PIN, MCP23X17_GPIO_OUTPUT);
  mcp23x17_set_mode(&mcp23x17_device_, MOTOR_A_BIN2_MCP_PIN, MCP23X17_GPIO_OUTPUT);
  mcp23x17_set_mode(&mcp23x17_device_, MOTOR_B_BIN1_MCP_PIN, MCP23X17_GPIO_OUTPUT);
  mcp23x17_set_mode(&mcp23x17_device_, MOTOR_B_BIN2_MCP_PIN, MCP23X17_GPIO_OUTPUT);
  mcp23x17_set_mode(&mcp23x17_device_, MOTOR_C_BIN1_MCP_PIN, MCP23X17_GPIO_OUTPUT);
  mcp23x17_set_mode(&mcp23x17_device_, MOTOR_C_BIN2_MCP_PIN, MCP23X17_GPIO_OUTPUT);
  mcp23x17_set_mode(&mcp23x17_device_, MOTOR_D_BIN1_MCP_PIN, MCP23X17_GPIO_OUTPUT);
  mcp23x17_set_mode(&mcp23x17_device_, MOTOR_D_BIN2_MCP_PIN, MCP23X17_GPIO_OUTPUT);

  set_motor_standby(true);
  motor_pwm_init();
  stop_soft();

  if (motor_control_task_handle_ == NULL) {
    xTaskCreatePinnedToCore(motor_control_task, "motor_ctrl", 4096, NULL, 3, &motor_control_task_handle_, 1);
  }
}


void UpdateLed(int64_t boottime_ms) {
  if (boottime_ms - led_updated_time_ > NO_MESSAGE_STOP_DELAY) {
    // printf("force stop\n");
    gpio_set_level(LED_PIN, LED_LIGHT_OFF);
    led_updated_time_ = boottime_ms;
    motor_lock();
    set_motion_target_locked(MOTION_STOP, 0);
    motor_unlock();
  }
}
