/*
 * ESPRESSIF MIT License
 *
 * Copyright (c) 2023 <ESPRESSIF SYSTEMS (SHANGHAI) CO., LTD>
 *
 * Permission is hereby granted for use on all ESPRESSIF SYSTEMS products, in which case,
 * it is free of charge, to any person obtaining a copy of this software and associated
 * documentation files (the "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the Software is furnished
 * to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or
 * substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#include "rtsp_service.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "string.h"

#include <esp_timer.h>

#include "media_lib_adapter.h"
#include "media_lib_netif.h"

static const char* TAG = "RTSP_SERVICE";

#ifdef CONFIG_CAMERA_OV5647_MIPI_RAW8_800X1280_50FPS
#define CAM_WIDTH 800
#define CAM_HEIGHT 1280
#elif defined(CONFIG_CAMERA_OV5647_MIPI_RAW8_800X640_50FPS)
#define CAM_CAPTURE_FMT V4L2_PIX_FMT_YUV420
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

#define VIDEO_FPS 30
#define VIDEO_MAX_SIZE (CAM_HEIGHT * CAM_WIDTH)
static esp_rtsp_mode_t rtsp_mode;
static uint32_t stream_first_pts;

static uint32_t get_cur_pts() {
  uint32_t cur_pts = 0;
  if (stream_first_pts == 0) {
    stream_first_pts = esp_timer_get_time() / 1000;
    cur_pts = 0;
  } else {
    cur_pts = esp_timer_get_time() / 1000 - stream_first_pts;
  }
  return cur_pts;
}

static char* rtsp_get_network_ip() {
  media_lib_ipv4_info_t ip_info;
  media_lib_netif_get_ipv4_info(MEDIA_LIB_NET_TYPE_STA, &ip_info);
  return media_lib_ipv4_ntoa(&ip_info.ip);
}

static int rtsp_esp_rtsp_state_handler(esp_rtsp_state_t state, void* ctx) {
  camera_context* camera_context = ctx;
  ESP_LOGD(TAG, "_esp_rtsp_state_handler state %d", state);
  switch ((int)state) {
    case RTSP_STATE_SETUP:
      ESP_LOGI(TAG, "RTSP_STATE_SETUP");
      break;
    case RTSP_STATE_PLAY: {
      stream_first_pts = 0;
      if (rtsp_mode == RTSP_CLIENT_PLAY) {
        return ESP_ERR_NOT_SUPPORTED;
      }
      video_start(CAM_WIDTH, CAM_HEIGHT, camera_context);
    }
      ESP_LOGI(TAG, "RTSP_STATE_PLAY");
      break;
    case RTSP_STATE_TEARDOWN: {
      stream_first_pts = 0;
      if (rtsp_mode == RTSP_CLIENT_PLAY) {
        return ESP_ERR_NOT_SUPPORTED;
      }
      video_stop(camera_context);
    }
      ESP_LOGI(TAG, "RTSP_STATE_TEARDOWN");
      break;
    default:
      return ESP_ERR_NOT_SUPPORTED;
  }
  return 0;
}

static int rtsp_send_audio(unsigned char* data, int len, uint32_t* pts, void* ctx) {
  *pts = get_cur_pts();
  return len;
}

static int rtsp_receive_audio(unsigned char* data, int len, void* ctx) { return len; }

static int rtsp_send_video(unsigned char* data, unsigned int* len, uint32_t* pts, void* ctx) {
  camera_context* camera_context = ctx;
  const frame_buffer_t* frame_buffer = video_fb_get(camera_context);
  memcpy(data, frame_buffer->buf, frame_buffer->len);
  *len = frame_buffer->len;
  video_after_take(camera_context);
  *pts = get_cur_pts();
  return ESP_OK;
}

esp_rtsp_handle_t rtsp_service_start(camera_context* av_stream) {
  ESP_ERROR_CHECK_WITHOUT_ABORT(av_stream == NULL);
  media_lib_add_default_adapter();

  esp_rtsp_video_info_t vcodec_info = {
      .vcodec = RTSP_VCODEC_H264,
      .width = CAM_HEIGHT,
      .height = CAM_WIDTH,
      .fps = VIDEO_FPS,
      .len = VIDEO_MAX_SIZE,
  };
  esp_rtsp_data_cb_t data_cb = {
      .send_audio = rtsp_send_audio,
      .receive_audio = rtsp_receive_audio,
      .send_video = rtsp_send_video,
  };
  esp_rtsp_config_t rtsp_config = {
      .mode = RTSP_SERVER,
      .ctx = av_stream,
      .data_cb = &data_cb,
      .audio_enable = false,
      .video_enable = true,
      .acodec = RTSP_ACODEC_G711A,
      .video_info = &vcodec_info,
      .local_addr = rtsp_get_network_ip(),
      .stack_size = RTSP_STACK_SZIE,
      .task_prio = RTSP_TASK_PRIO,
      .state = rtsp_esp_rtsp_state_handler,
      .trans = RTSP_TRANSPORT_TCP,
  };

  rtsp_config.local_port = RTSP_SERVER_PORT;
  return esp_rtsp_server_start(&rtsp_config);
}

int rtsp_service_stop(esp_rtsp_handle_t esp_rtsp) {
  if (esp_rtsp) {
    if (rtsp_mode == RTSP_SERVER) {
      esp_rtsp_server_stop(esp_rtsp);
    } else {
      esp_rtsp_client_stop(esp_rtsp);
    }
  }
  return ESP_FAIL;
}
