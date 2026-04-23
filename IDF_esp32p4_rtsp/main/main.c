/* RTSP Example

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
#include "rtsp_service.h"
#include "video_dev.h"
#include "wifi_station.h"

#define TAG "ESP_RTSP_Demo"

static esp_rtsp_handle_t esp_rtsp;
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
  video_dev_init(&g_camera_context);

  ESP_LOGI(TAG, "[ 2 ] Initialize rtsp_service_start");
  esp_rtsp = rtsp_service_start(&g_camera_context);
}
