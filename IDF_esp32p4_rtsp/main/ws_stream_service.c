#include "ws_stream_service.h"

#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define WS_SERVER_PORT 8080
#define WS_URI "/ws"
#define WS_PKT_MAGIC 0x5753494DU
#define WS_PKT_VER 3U

typedef struct __attribute__((packed)) {
  uint16_t x;
  uint16_t y;
  uint32_t score;
} ws_point_t;

typedef struct __attribute__((packed)) {
  uint32_t magic;
  uint16_t version;
  uint16_t point_count;
  uint32_t frame_id;
  uint16_t small_width;
  uint16_t small_height;
  uint32_t small_size;
} ws_frame_header_t;

typedef struct {
  httpd_handle_t server;
  int client_fd;
  TaskHandle_t stream_task;
  bool video_started;
  camera_context* camera;
} ws_service_ctx_t;

static const char* TAG = "WS_STREAM";
static ws_service_ctx_t s_ctx;

#if CONFIG_ESP_VIDEO_ENABLE_USB_UVC_VIDEO_DEVICE
#define CAM_WIDTH 640
#define CAM_HEIGHT 480
#else

#ifdef CONFIG_CAMERA_OV5647_MIPI_RAW8_800X1280_50FPS
#define CAM_WIDTH 800
#define CAM_HEIGHT 1280
#elif defined(CONFIG_CAMERA_OV5647_MIPI_RAW8_800X640_50FPS)
#define CAM_WIDTH 800
#define CAM_HEIGHT 640
#elif defined(CONFIG_CAMERA_OV5647_MIPI_RAW8_800X800_50FPS) || defined(CONFIG_CAMERA_SC2336_MIPI_RAW8_800X800_30FPS)
#define CAM_WIDTH 800
#define CAM_HEIGHT 800
#elif defined(CONFIG_CAMERA_SC2336_MIPI_RAW8_1024x600_30FPS)
#define CAM_WIDTH 1024
#define CAM_HEIGHT 600
#elif defined(CONFIG_CAMERA_SC2336_MIPI_RAW8_1280x720_30FPS)
#define CAM_WIDTH 1280
#define CAM_HEIGHT 720
#elif defined(CONFIG_CAMERA_OV5647_MIPI_RAW10_1280x960_BINNING_45FPS)
#define CAM_WIDTH 1280
#define CAM_HEIGHT 960
#elif defined(CONFIG_CAMERA_SC2336_MIPI_RAW8_1920X1080_30FPS) || \
    defined(CONFIG_CAMERA_OV5647_MIPI_RAW10_1920x1080_30FPS)
#define CAM_WIDTH 1920
#define CAM_HEIGHT 1080
#endif

#endif

static void ws_stream_task(void* arg) {
  (void)arg;
  while (s_ctx.client_fd >= 0 && s_ctx.server) {
    frame_buffer_t* enc = video_fb_get(s_ctx.camera);
    const frame_buffer_t* raw = video_raw_fb_get(s_ctx.camera);
    size_t small_w = 0, small_h = 0, small_len = 0;
    const uint8_t* small_img = video_quarter_image_get(s_ctx.camera, &small_w, &small_h, &small_len);
    size_t pts_count = 0;
    const corner_point_t* pts = video_corner_points_get(s_ctx.camera, &pts_count);
    if (!enc || !raw || !pts || !small_img) {
      continue;
    }
    if (pts_count > MAX_CORNER_POINTS) {
      pts_count = MAX_CORNER_POINTS;
    }

    size_t payload_len = sizeof(ws_frame_header_t) + pts_count * sizeof(ws_point_t) + small_len;
    uint8_t* payload = (uint8_t*)malloc(payload_len);
    if (!payload) {
      video_after_take(s_ctx.camera);
      vTaskDelay(pdMS_TO_TICKS(5));
      continue;
    }

    ws_frame_header_t header = {
        .magic = WS_PKT_MAGIC,
        .version = WS_PKT_VER,
        .point_count = (uint16_t)pts_count,
        .frame_id = raw->frame_id,
        .small_width = (uint16_t)small_w,
        .small_height = (uint16_t)small_h,
        .small_size = (uint32_t)small_len,
    };
    memcpy(payload, &header, sizeof(header));
    memcpy(payload + sizeof(header), pts, pts_count * sizeof(ws_point_t));
    memcpy(payload + sizeof(header) + pts_count * sizeof(ws_point_t), small_img, small_len);

    httpd_ws_frame_t ws_pkt = {
        .final = true,
        .fragmented = false,
        .type = HTTPD_WS_TYPE_BINARY,
        .payload = payload,
        .len = payload_len,
    };
    esp_err_t err = httpd_ws_send_frame_async(s_ctx.server, s_ctx.client_fd, &ws_pkt);
    free(payload);
    video_after_take(s_ctx.camera);

    if (err != ESP_OK) {
      ESP_LOGW(TAG, "ws send failed (%d), drop client", err);
      s_ctx.client_fd = -1;
      break;
    }
  }

  if (s_ctx.video_started) {
    video_stop(s_ctx.camera);
    s_ctx.video_started = false;
  }
  s_ctx.stream_task = NULL;
  vTaskDelete(NULL);
}

static esp_err_t ws_handler(httpd_req_t* req) {
  if (req->method == HTTP_GET) {
    s_ctx.client_fd = httpd_req_to_sockfd(req);
    ESP_LOGI(TAG, "ws client connected fd=%d", s_ctx.client_fd);

    if (!s_ctx.video_started) {
      esp_err_t ret = video_start(CAM_WIDTH, CAM_HEIGHT, s_ctx.camera);
      if (ret != ESP_OK) {
        ESP_LOGE(TAG, "video_start failed: %s", esp_err_to_name(ret));
        return ret;
      }
      s_ctx.video_started = true;
    }
    if (!s_ctx.stream_task) {
      xTaskCreate(ws_stream_task, "ws_stream_task", 12288, NULL, 5, &s_ctx.stream_task);
    }
    return ESP_OK;
  }

  httpd_ws_frame_t frame = {0};
  frame.type = HTTPD_WS_TYPE_TEXT;
  return httpd_ws_recv_frame(req, &frame, 0);
}

esp_err_t ws_stream_service_start(camera_context* camera_context) {
  if (!camera_context) {
    return ESP_ERR_INVALID_ARG;
  }
  memset(&s_ctx, 0, sizeof(s_ctx));
  s_ctx.client_fd = -1;
  s_ctx.camera = camera_context;

  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = WS_SERVER_PORT;
  config.max_open_sockets = 4;

  ESP_ERROR_CHECK(httpd_start(&s_ctx.server, &config));
  httpd_uri_t ws_uri = {
      .uri = WS_URI,
      .method = HTTP_GET,
      .handler = ws_handler,
      .user_ctx = NULL,
      .is_websocket = true,
  };
  ESP_ERROR_CHECK(httpd_register_uri_handler(s_ctx.server, &ws_uri));

  ESP_LOGI(TAG, "ws stream started: ws://<board-ip>:%d%s", WS_SERVER_PORT, WS_URI);
  return ESP_OK;
}

esp_err_t ws_stream_service_stop(void) {
  s_ctx.client_fd = -1;
  if (s_ctx.stream_task) {
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  if (s_ctx.server) {
    httpd_stop(s_ctx.server);
    s_ctx.server = NULL;
  }
  return ESP_OK;
}
