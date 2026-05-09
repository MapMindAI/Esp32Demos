

#include "control/control_callback.h"
// #include "driver/adc.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include <mcp23x17.h>


#define TAG "[CONTORL]"

// static EventGroupHandle_t event_grounp_handle_ = NULL;
static mcp23x17_t mcp23x17_device_ = { 0 };

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
  mcp23x17_set_level(&mcp23x17_device_, MOTOR_A_BIN1_MCP_PIN, front ? 0 : 1);
  mcp23x17_set_level(&mcp23x17_device_, MOTOR_A_BIN2_MCP_PIN, front ? 1 : 0);
}

static void set_b_direction(bool front) {
  mcp23x17_set_level(&mcp23x17_device_, MOTOR_B_BIN1_MCP_PIN, front ? 0 : 1);
  mcp23x17_set_level(&mcp23x17_device_, MOTOR_B_BIN2_MCP_PIN, front ? 1 : 0);
}

static void set_c_direction(bool front) {
  mcp23x17_set_level(&mcp23x17_device_, MOTOR_C_BIN1_MCP_PIN, front ? 0 : 1);
  mcp23x17_set_level(&mcp23x17_device_, MOTOR_C_BIN2_MCP_PIN, front ? 1 : 0);
}

static void set_d_direction(bool front) {
  mcp23x17_set_level(&mcp23x17_device_, MOTOR_D_BIN1_MCP_PIN, front ? 0 : 1);
  mcp23x17_set_level(&mcp23x17_device_, MOTOR_D_BIN2_MCP_PIN, front ? 1 : 0);
}

static void forward(int speed) {
  set_a_direction(1);
  set_b_direction(1);
  set_c_direction(1);
  set_d_direction(1);
  motor_set_speed(speed, speed, speed, speed);
}

static void backward(int speed) {
  set_a_direction(0);
  set_b_direction(0);
  set_c_direction(0);
  set_d_direction(0);
  motor_set_speed(speed, speed, speed, speed);
}

static void turn_left(int speed) {
  set_a_direction(1);
  set_b_direction(0);
  set_c_direction(1);
  set_d_direction(0);
  motor_set_speed(speed, speed, speed, speed);
}

static void turn_right(int speed) {
  set_a_direction(0);
  set_b_direction(1);
  set_c_direction(0);
  set_d_direction(1);
  motor_set_speed(speed, speed, speed, speed);
}

static void stop() { motor_set_speed(0, 0, 0, 0); }

static int64_t led_updated_time_ = 0;

void ControlMessageCallback(int len, const uint8_t* value) {
  uint8_t message_type = value[0];
  switch (message_type) {
    case 66:
      esp_restart();  // Reboots the ESP32
      break;
    case 8: {
      ESP_LOGI(TAG, "move front");
      forward(125);
    } break;
    case 2: {
      ESP_LOGI(TAG, "move back");
      backward(125);
    } break;
    case 4: {
      ESP_LOGI(TAG, "move left");
      turn_left(125);
    } break;
    case 5: {
      ESP_LOGI(TAG, "stop");
      stop();
    } break;
    case 6: {
      ESP_LOGI(TAG, "move right");
      turn_right(125);
    } break;
    default: {
      ESP_LOG_BUFFER_HEX(TAG, value, len);
    } break;
  }
  led_updated_time_ = esp_timer_get_time();
  gpio_set_level(LED_PIN, LED_LIGHT_ON);
}

void CansbusControlMessageCallback(int len, const uint8_t* value) {
  uint8_t message_type = value[0];
  switch (message_type) {
    case 66:
      esp_restart();  // Reboots the ESP32
      break;
    case 8: {
      ESP_LOGI(TAG, "move front");
      forward(125);
    } break;
    case 2: {
      ESP_LOGI(TAG, "move back");
      backward(125);
    } break;
    case 4: {
      ESP_LOGI(TAG, "move left");
      turn_left(125);
    } break;
    case 5: {
      ESP_LOGI(TAG, "stop");
      stop();
    } break;
    case 6: {
      ESP_LOGI(TAG, "move right");
      turn_right(125);
    } break;
    default: {
      ESP_LOG_BUFFER_HEX(TAG, value, len);
    } break;
  }
  led_updated_time_ = esp_timer_get_time();
  gpio_set_level(LED_PIN, LED_LIGHT_ON);
}

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
  motor_pwm_init();
  stop();

  // enable standby
  mcp23x17_set_level(&mcp23x17_device_, MOTOR_STBY_MCP_PIN, true);
}


void UpdateLed(int64_t boottime_ms) {
  if (boottime_ms - led_updated_time_ > 500000) {
    // printf("force stop\n");
    gpio_set_level(LED_PIN, LED_LIGHT_OFF);
    led_updated_time_ = boottime_ms;
    stop();
  }
}
