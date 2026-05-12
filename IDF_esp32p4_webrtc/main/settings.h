/* General settings

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#pragma once

#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  Board name setting refer to `codec_board` README.md for more details
 */
#if CONFIG_IDF_TARGET_ESP32P4
#define TEST_BOARD_NAME "ESP32_P4_DEV_V14"
#else
#define TEST_BOARD_NAME "S3_Korvo_V2"
#endif

/**
 * @brief  Video resolution settings
 */
#if CONFIG_IDF_TARGET_ESP32P4
// #define VIDEO_WIDTH  1920
// #define VIDEO_HEIGHT 1080
#define VIDEO_FPS 25

#ifdef CONFIG_CAMERA_OV5647_MIPI_RAW8_800X1280_50FPS
#define VIDEO_WIDTH 800
#define VIDEO_HEIGHT 1280
#elif defined(CONFIG_CAMERA_OV5647_MIPI_RAW8_800X640_50FPS)
#define VIDEO_WIDTH 800
#define VIDEO_HEIGHT 640
#elif defined(CONFIG_CAMERA_OV5647_MIPI_RAW8_800X800_50FPS)
#define VIDEO_WIDTH 800
#define VIDEO_HEIGHT 800
#elif defined(CONFIG_CAMERA_OV5647_MIPI_RAW10_1920X1080_30FPS)
#define VIDEO_WIDTH 1920
#define VIDEO_HEIGHT 1080
#elif defined(CONFIG_CAMERA_OV5647_MIPI_RAW10_1280X960_BINNING_45FPS)
#define VIDEO_WIDTH 1280
#define VIDEO_HEIGHT 720
#endif

#else
#define VIDEO_WIDTH 320
#define VIDEO_HEIGHT 240
#define VIDEO_FPS 10
#endif

/**
 * @brief  Set for wifi ssid
 */
#define WIFI_SSID CONFIG_DOORBELL_WIFI_SSID

/**
 * @brief  Set for wifi password
 */
#define WIFI_PASSWORD CONFIG_DOORBELL_WIFI_PASSWORD

/**
 * @brief  Whether enable data channel
 */
#define DATA_CHANNEL_ENABLED (true)
/* Disable data-channel stress traffic by default to keep video stable. */
#define DATA_CHANNEL_STRESS_TEST (false)

/* Conservative WebRTC bitrate profile for higher stability on unstable links. */
#define WEBRTC_VIDEO_BITRATE_START (450000)
#define WEBRTC_VIDEO_BITRATE_STABLE (700000)
#define WEBRTC_AUDIO_BITRATE (20000)

/**
 * @brief MQTT settings for local broker tests
 */
#define MQTT_BROKER_URI CONFIG_DOORBELL_MQTT_BROKER_URI
#define STR_HELPER(x) #x
#define STR(x) STR_HELPER(x)
#define MQTT_USERNAME STR(CONFIG_DOORBELL_JANUS_ROOM_ID)
#define MQTT_MEM_TOPIC MQTT_USERNAME "/memory"
#define MQTT_CMD_TOPIC MQTT_USERNAME "/command"

#if CONFIG_DOORBELL_SIGNALING_JANUS
#define WEBRTC_SIGNAL_URL CONFIG_DOORBELL_JANUS_SIGNAL_URL
#define WEBRTC_JANUS_ROOM_ID CONFIG_DOORBELL_JANUS_ROOM_ID
#define WEBRTC_JANUS_ROOM_PIN CONFIG_DOORBELL_JANUS_ROOM_PIN
#define WEBRTC_JANUS_DISPLAY CONFIG_DOORBELL_JANUS_DISPLAY
#define WEBRTC_JANUS_TOKEN CONFIG_DOORBELL_JANUS_TOKEN
#define WEBRTC_JANUS_API_SECRET CONFIG_DOORBELL_JANUS_API_SECRET

// Reuse Janus room PIN as MQTT password to simplify management.
#define MQTT_PASSWORD WEBRTC_JANUS_ROOM_PIN

#else
#define MQTT_PASSWORD "CHANGE_ME_MQTT_PASSWORD"
#endif

#if CONFIG_IDF_TARGET_ESP32P4
/**
 * @brief  GPIO for ring button
 *
 * @note  When use ESP32P4-Fuction-Ev-Board, GPIO35(boot button) is connected RMII_TXD1
 *        When enable `NETWORK_USE_ETHERNET` will cause socket error
 *        User must replace it to a unused GPIO instead (like GPIO27)
 */
#define DOOR_BELL_RING_BUTTON 35
#else
/**
 * @brief  GPIO for ring button
 *
 * @note  When use ESP32S3-KORVO-V3 Use ADC button as ring button
 */
#define DOOR_BELL_RING_BUTTON 5
#endif

#ifdef __cplusplus
}
#endif
