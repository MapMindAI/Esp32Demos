/* WebSocket Stream Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <esp_event.h>
#include <mdns.h>
// #include <protocol_examples_common.h>
#include <lwip/apps/netbiosns.h>

#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "video_dev.h"
#include "wifi_station.h"
#include "ws_stream_service.h"

#define TAG "ESP_WS_Demo"

static camera_context g_camera_context;

static void net_connect(void) {
  ESP_LOGI(TAG, "[ 0 ] connect network");
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    // NVS partition was truncated and needs to be erased
    // Retry nvs_flash_init
    ESP_ERROR_CHECK(nvs_flash_erase());
    ESP_ERROR_CHECK(nvs_flash_init());
  }
  wifi_init_sta();
}

void app_main() {
  net_connect();

  ESP_LOGI(TAG, "[ 1 ] Initialize av stream");
  esp_err_t ret = video_dev_init(&g_camera_context);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "video_dev_init failed: %s", esp_err_to_name(ret));
    return;
  }

  ESP_LOGI(TAG, "[ 2 ] Start websocket stream service");
  ESP_ERROR_CHECK(ws_stream_service_start(&g_camera_context));
}
