#include "mqtt_bridge.h"
#include <inttypes.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include "settings.h"

static const char *TAG = "MQTT_BRIDGE";
static esp_mqtt_client_handle_t s_client;
static TaskHandle_t s_pub_task;
static bool s_connected;
static bool s_started;
static mqtt_bridge_cmd_handler_t s_cmd_handler;
static void *s_cmd_ctx;

static int publish_raw(const char *topic, const char *payload)
{
  if (!s_connected || !s_client || !topic || !payload) {
    return -1;
  }
  return esp_mqtt_client_publish(s_client, topic, payload, 0, 0, 0);
}

static void mqtt_pub_task(void *arg)
{
  char payload[192];
  while (s_started) {
    if (s_connected && s_client) {
      int free_heap = esp_get_free_heap_size();
      int free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
      int64_t uptime_ms = esp_timer_get_time() / 1000;
      int n = snprintf(payload, sizeof(payload),
                       "{\"uptime_ms\":%" PRIi64 ",\"free_heap\":%d,\"free_psram\":%d}",
                       uptime_ms, free_heap, free_psram);
      esp_mqtt_client_publish(s_client, MQTT_MEM_TOPIC, payload, n, 0, 0);
    }
    vTaskDelay(pdMS_TO_TICKS(2000));
  }
  vTaskDelete(NULL);
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id,
                               void *event_data)
{
  esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
  if (event_id == MQTT_EVENT_CONNECTED) {
    s_connected = true;
    ESP_LOGI(TAG, "Connected to broker: %s", MQTT_BROKER_URI);
    esp_mqtt_client_subscribe(s_client, MQTT_CMD_TOPIC, 0);
    esp_mqtt_client_publish(s_client, MQTT_MEM_TOPIC, "{\"status\":\"online\"}", 0, 0, 0);
  } else if (event_id == MQTT_EVENT_DISCONNECTED) {
    s_connected = false;
    ESP_LOGW(TAG, "Disconnected from broker");
  } else if (event_id == MQTT_EVENT_SUBSCRIBED) {
    ESP_LOGI(TAG, "Subscribed topic: %s", MQTT_CMD_TOPIC);
  } else if (event_id == MQTT_EVENT_DATA) {
    if (event->topic_len == (int)strlen(MQTT_CMD_TOPIC) &&
        strncmp(event->topic, MQTT_CMD_TOPIC, event->topic_len) == 0) {
      int copy_len = event->data_len > 200 ? 200 : event->data_len;
      char cmd[201];
      memcpy(cmd, event->data, copy_len);
      cmd[copy_len] = '\0';
      ESP_LOGI(TAG, "MQTT command received: %s", cmd);
      if (s_cmd_handler) {
        s_cmd_handler(cmd, s_cmd_ctx);
      }
    }
  } else if (event_id == MQTT_EVENT_ERROR) {
    ESP_LOGE(TAG, "MQTT error event");
  }
}

int mqtt_bridge_start(void)
{
  if (s_started) {
    return 0;
  }
  const esp_mqtt_client_config_t mqtt_cfg = {
      .broker.address.uri = MQTT_BROKER_URI,
      .credentials.username = MQTT_USERNAME,
      .credentials.authentication.password = MQTT_PASSWORD,
  };
  s_client = esp_mqtt_client_init(&mqtt_cfg);
  if (s_client == NULL) {
    ESP_LOGE(TAG, "Failed to init MQTT client");
    return -1;
  }
  s_started = true;
  s_connected = false;
  esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
  esp_mqtt_client_start(s_client);
  xTaskCreate(mqtt_pub_task, "mqtt_pub_task", 4096, NULL, 5, &s_pub_task);
  ESP_LOGI(TAG, "MQTT bridge started");
  return 0;
}

void mqtt_bridge_stop(void)
{
  if (!s_started) {
    return;
  }
  s_started = false;
  s_connected = false;
  if (s_client) {
    esp_mqtt_client_stop(s_client);
    esp_mqtt_client_destroy(s_client);
    s_client = NULL;
  }
  s_pub_task = NULL;
  ESP_LOGI(TAG, "MQTT bridge stopped");
}

void mqtt_bridge_set_cmd_handler(mqtt_bridge_cmd_handler_t handler, void *ctx)
{
  s_cmd_handler = handler;
  s_cmd_ctx = ctx;
}

int mqtt_bridge_publish_json(const char *topic_suffix, const char *json)
{
  if (!topic_suffix || !json) {
    return -1;
  }
  char topic[128];
  snprintf(topic, sizeof(topic), "%s/%s", MQTT_USERNAME, topic_suffix);
  return publish_raw(topic, json);
}

bool mqtt_bridge_is_connected(void)
{
  return s_connected;
}
