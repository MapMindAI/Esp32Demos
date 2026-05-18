#ifndef VIDEO_DEV_H
#define VIDEO_DEV_H

#include <esp_cam_sensor_types.h>
#include <esp_err.h>
#include <linux/videodev2.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if CONFIG_ESP_VIDEO_ENABLE_USB_UVC_VIDEO_DEVICE
#define CAM_DEV_PATH ESP_VIDEO_USB_UVC_DEVICE_NAME(0)
#else
#define CAM_DEV_PATH ESP_VIDEO_MIPI_CSI_DEVICE_NAME
#endif
#define ENCODE_DEV_PATH ESP_VIDEO_JPEG_DEVICE_NAME

#define EXAMPLE_VIDEO_BUFFER_COUNT 2
#define BUFFER_COUNT EXAMPLE_VIDEO_BUFFER_COUNT

typedef struct {
  uint8_t* buf;
  size_t len;
  size_t width;
  size_t height;
  uint32_t frame_id;
  struct timeval timestamp;
} frame_buffer_t;

typedef struct {
  uint16_t x;
  uint16_t y;
  uint32_t score;
} corner_point_t;

#define MAX_CORNER_POINTS 1024

typedef struct {
  int cap_fd;
  uint32_t format;
  uint32_t cap_pixfmt;
  uint8_t* cap_buffer[BUFFER_COUNT];
  size_t cap_width;
  size_t cap_height;
  frame_buffer_t raw_fb;
  corner_point_t corners[MAX_CORNER_POINTS];
  size_t corners_num;
  uint8_t* quarter_img;
  size_t quarter_width;
  size_t quarter_height;
  size_t quarter_len;
  bool direct_jpeg;
  void* jpeg_dec;
  uint8_t* jpeg_in_buf;
  size_t jpeg_in_buf_size;
  uint8_t* jpeg_gray_buf;
  size_t jpeg_gray_buf_size;
  size_t jpeg_gray_width;
  size_t jpeg_gray_height;
  int m2m_fd;
  uint8_t* m2m_cap_buffer;
  frame_buffer_t fb;
} camera_context;

esp_err_t video_dev_init(camera_context* context);

esp_err_t video_start(int width, int height, camera_context* cb_ctx);

frame_buffer_t* video_fb_get(camera_context* cb_ctx);

const frame_buffer_t* video_raw_fb_get(const camera_context* cb_ctx);

const corner_point_t* video_corner_points_get(const camera_context* cb_ctx, size_t* count);
const uint8_t* video_quarter_image_get(const camera_context* cb_ctx, size_t* width, size_t* height, size_t* len);

uint32_t video_frame_id_get(const camera_context* cb_ctx);

void video_after_take(const camera_context* cb_ctx);

void video_stop(camera_context* cb_ctx);

void enumerate_camera_capabilities(int fd);

#endif
