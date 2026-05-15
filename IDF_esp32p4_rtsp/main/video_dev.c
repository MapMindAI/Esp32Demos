/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: ESPRESSIF MIT
 */

#include "video_dev.h"
#include "config.h"
#include <fcntl.h>
#include <string.h>
#include <sys/errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/param.h>
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_video_device.h"
#include "esp_video_init.h"
#include "linux/videodev2.h"

#include <esp_heap_caps.h>
#include <esp_system.h>
#include <inttypes.h>
#include <math.h>

#define OUTPUT_FORMAT V4L2_PIX_FMT_JPEG
#define CAM_DEV_PATH ESP_VIDEO_MIPI_CSI_DEVICE_NAME
#define DIS_VIDEO_CODEC_TIMESTAMP 1

#if CONFIG_EXAMPLE_H264_MAX_QP <= CONFIG_EXAMPLE_H264_MIN_QP
#error "CONFIG_EXAMPLE_H264_MAX_QP should larger than CONFIG_EXAMPLE_H264_MIN_QP"
#endif

static const char* TAG = "camera_driver";

static bool format_change = true;
static uint32_t frame_seq;

static const esp_video_init_csi_config_t csi_config[] = {
    {
        .sccb_config =
            {
                .init_sccb = true,
                .i2c_config =
                    {
                        .port = CONFIG_EXAMPLE_MIPI_CSI_SCCB_I2C_PORT,
                        .scl_pin = CONFIG_EXAMPLE_MIPI_CSI_SCCB_I2C_SCL_PIN,
                        .sda_pin = CONFIG_EXAMPLE_MIPI_CSI_SCCB_I2C_SDA_PIN,
                    },
                .freq = CONFIG_EXAMPLE_MIPI_CSI_SCCB_I2C_FREQ,
            },
        .reset_pin = CONFIG_EXAMPLE_MIPI_CSI_CAM_SENSOR_RESET_PIN,
        .pwdn_pin = CONFIG_EXAMPLE_MIPI_CSI_CAM_SENSOR_PWDN_PIN,
    },
};

static const esp_video_init_config_t cam_config = {
    .csi = csi_config,
};

static inline uint8_t rgb_to_gray_u8(int r, int g, int b) {
  // ITU-R BT.601 integer approximation
  int y = (77 * r + 150 * g + 29 * b) >> 8;
  if (y < 0) y = 0;
  if (y > 255) y = 255;
  return (uint8_t)y;
}

static inline uint8_t raw_gray_at(const camera_context* cb_ctx, const uint8_t* raw, int x, int y, int w, int h) {
  if (x < 0) x = 0;
  if (y < 0) y = 0;
  if (x >= w) x = w - 1;
  if (y >= h) y = h - 1;

  switch (cb_ctx->cap_pixfmt) {
    case V4L2_PIX_FMT_GREY:
      return raw[y * w + x];
    case V4L2_PIX_FMT_YUV420:
    case V4L2_PIX_FMT_YUV422P:
      // Y plane is the first width*height bytes for both planar formats.
      return raw[y * w + x];
    case V4L2_PIX_FMT_RGB24: {
      int idx = (y * w + x) * 3;
      int r = raw[idx + 0];
      int g = raw[idx + 1];
      int b = raw[idx + 2];
      return rgb_to_gray_u8(r, g, b);
    }
    case V4L2_PIX_FMT_RGB565: {
      int idx = (y * w + x) * 2;
      uint16_t p = (uint16_t)raw[idx] | ((uint16_t)raw[idx + 1] << 8);
      int r = ((p >> 11) & 0x1F) * 255 / 31;
      int g = ((p >> 5) & 0x3F) * 255 / 63;
      int b = (p & 0x1F) * 255 / 31;
      return rgb_to_gray_u8(r, g, b);
    }
    default:
      // Fallback: treat as luma-like single channel.
      return raw[y * w + x];
  }
}

static const float kDirs[16][2] = {
    {0.0f, 1.0f},       {0.3827f, 0.9239f},  {0.1951f, 0.9808f},   {0.9239f, 0.3827f},
    {0.7071f, 0.7071f}, {0.3827f, -0.9239f}, {0.8315f, 0.5556f},   {0.8315f, -0.5556f},
    {0.5556f, -0.8315f},{0.9808f, 0.1951f},  {0.9239f, -0.3827f},  {0.7071f, -0.7071f},
    {0.5556f, 0.8315f}, {0.9808f, -0.1951f}, {1.0f, 0.0f},         {0.1951f, -0.9808f},
};

static void run_pixel_selector_quarter(camera_context* cb_ctx, const uint8_t* y_plane, size_t width, size_t height) {
  cb_ctx->corners_num = 0;
  if (!y_plane || !cb_ctx->quarter_img || cb_ctx->quarter_width < 4 || cb_ctx->quarter_height < 4) {
    return;
  }

  const int qw = (int)cb_ctx->quarter_width;
  const int qh = (int)cb_ctx->quarter_height;
  const int src_w = (int)width;
  const int src_h = (int)height;
  const int scale = SELECTOR_SMALL_IMG_SCALE;

  for (int y = 0; y < qh; ++y) {
    int sy = y * scale;
    for (int x = 0; x < qw; ++x) {
      int sx = x * scale;
      uint32_t ssum = 0;
      for (int by = 0; by < scale; ++by) {
        for (int bx = 0; bx < scale; ++bx) {
          ssum += raw_gray_at(cb_ctx, y_plane, sx + bx, sy + by, src_w, src_h);
        }
      }
      cb_ctx->quarter_img[y * qw + x] = (uint8_t)(ssum / (uint32_t)(scale * scale));
    }
  }

  const int blk = SELECTOR_BLOCK_SIZE;
  const int w32 = (qw + blk - 1) / blk;
  const int h32 = (qh + blk - 1) / blk;
  const int gradient_histogram_add = SELECTOR_GRAD_HIST_ADD;
  const float threshold_scale = SELECTOR_THRESHOLD_SCALE;
  const float best_val_min = SELECTOR_BEST_VAL_MIN;
  int* th_hist = (int*)heap_caps_malloc((size_t)w32 * h32 * sizeof(int), MALLOC_CAP_8BIT);
  float* th_smooth = (float*)heap_caps_malloc((size_t)w32 * h32 * sizeof(float), MALLOC_CAP_8BIT);
  if (!th_hist || !th_smooth) {
    if (th_hist) free(th_hist);
    if (th_smooth) free(th_smooth);
    return;
  }

  for (int by = 0; by < h32; ++by) {
    for (int bx = 0; bx < w32; ++bx) {
      int hist[51] = {0};
      int xs = bx * blk, ys = by * blk;
      int xe = xs + blk > qw ? qw : xs + blk;
      int ye = ys + blk > qh ? qh : ys + blk;
      int cnt = 0;
      for (int y = ys; y < ye; ++y) {
        for (int x = xs; x < xe; ++x) {
          int gg = 0;
          if (x > 0 && x < qw - 1 && y > 0 && y < qh - 1) {
            int idx = y * qw + x;
            int dx = (int)cb_ctx->quarter_img[idx + 1] - (int)cb_ctx->quarter_img[idx - 1];
            int dy = (int)cb_ctx->quarter_img[idx + qw] - (int)cb_ctx->quarter_img[idx - qw];
            gg = (int)sqrtf((float)(dx * dx + dy * dy));
          }
          if (gg > 48) gg = 48;
          hist[gg + 1]++;
          cnt++;
        }
      }
      hist[0] = cnt;
      int th = (int)(hist[0] * 0.5f + 0.5f);
      int q = 50;
      for (int i = 0; i < 50; ++i) {
        th -= hist[i + 1];
        if (th < 0) {
          q = i;
          break;
        }
      }
      th_hist[bx + by * w32] = q + gradient_histogram_add;
    }
  }

  for (int by = 0; by < h32; ++by) {
    for (int bx = 0; bx < w32; ++bx) {
      int sum = 0, num = 0;
      for (int oy = -1; oy <= 1; ++oy) {
        for (int ox = -1; ox <= 1; ++ox) {
          int nx = bx + ox, ny = by + oy;
          if (nx < 0 || ny < 0 || nx >= w32 || ny >= h32) continue;
          sum += th_hist[nx + ny * w32];
          num++;
        }
      }
      float a = (float)sum / (float)num;
      th_smooth[bx + by * w32] = a * a;
    }
  }

  const int potential = SELECTOR_POTENTIAL;
  for (int yb = 1; yb < qh - potential; yb += potential) {
    for (int xb = 1; xb < qw - potential; xb += potential) {
      if (cb_ctx->corners_num >= MAX_CORNER_POINTS) break;
      int best_idx = -1;
      float best_val = 0.0f;
      const float* dir = kDirs[(cb_ctx->corners_num + frame_seq) & 15];
      for (int dy0 = 0; dy0 < potential; ++dy0) {
        for (int dx0 = 0; dx0 < potential; ++dx0) {
          int x = xb + dx0;
          int y = yb + dy0;
          int idx = y * qw + x;
          float th = th_smooth[(x >> 5) + (y >> 5) * w32] * threshold_scale;
          int dx = (int)cb_ctx->quarter_img[idx + 1] - (int)cb_ctx->quarter_img[idx - 1];
          int dy = (int)cb_ctx->quarter_img[idx + qw] - (int)cb_ctx->quarter_img[idx - qw];
          float g2 = (float)(dx * dx + dy * dy);
          if (g2 > th) {
            float proj = fabsf((float)dx * dir[0] + (float)dy * dir[1]);
            if (proj > best_val) {
              best_val = proj;
              best_idx = idx;
            }
          }
        }
      }
      if (best_idx >= 0 && best_val >= best_val_min) {
        cb_ctx->corners[cb_ctx->corners_num].x = (uint16_t)(best_idx % qw);
        cb_ctx->corners[cb_ctx->corners_num].y = (uint16_t)(best_idx / qw);
        cb_ctx->corners[cb_ctx->corners_num].score = (uint32_t)best_val;
        cb_ctx->corners_num++;
      }
    }
  }

  free(th_hist);
  free(th_smooth);
}

static void print_video_device_info(const struct v4l2_capability* capability) {
  ESP_LOGI(TAG, "version: %d.%d.%d", (uint16_t)(capability->version >> 16), (uint8_t)(capability->version >> 8),
           (uint8_t)capability->version);
  ESP_LOGI(TAG, "driver:  %s", capability->driver);
  ESP_LOGI(TAG, "card:    %s", capability->card);
  ESP_LOGI(TAG, "bus:     %s", capability->bus_info);
  ESP_LOGI(TAG, "capabilities:");
  if (capability->capabilities & V4L2_CAP_VIDEO_CAPTURE) {
    ESP_LOGI(TAG, "\tVIDEO_CAPTURE");
  }
  if (capability->capabilities & V4L2_CAP_READWRITE) {
    ESP_LOGI(TAG, "\tREADWRITE");
  }
  if (capability->capabilities & V4L2_CAP_ASYNCIO) {
    ESP_LOGI(TAG, "\tASYNCIO");
  }
  if (capability->capabilities & V4L2_CAP_STREAMING) {
    ESP_LOGI(TAG, "\tSTREAMING");
  }
  if (capability->capabilities & V4L2_CAP_META_OUTPUT) {
    ESP_LOGI(TAG, "\tMETA_OUTPUT");
  }
  if (capability->capabilities & V4L2_CAP_DEVICE_CAPS) {
    ESP_LOGI(TAG, "device capabilities:");
    if (capability->device_caps & V4L2_CAP_VIDEO_CAPTURE) {
      ESP_LOGI(TAG, "\tVIDEO_CAPTURE");
    }
    if (capability->device_caps & V4L2_CAP_READWRITE) {
      ESP_LOGI(TAG, "\tREADWRITE");
    }
    if (capability->device_caps & V4L2_CAP_ASYNCIO) {
      ESP_LOGI(TAG, "\tASYNCIO");
    }
    if (capability->device_caps & V4L2_CAP_STREAMING) {
      ESP_LOGI(TAG, "\tSTREAMING");
    }
    if (capability->device_caps & V4L2_CAP_META_OUTPUT) {
      ESP_LOGI(TAG, "\tMETA_OUTPUT");
    }
  }
}

static esp_err_t init_capture_video(camera_context* context) {
  int fd;
  struct v4l2_capability capability;

  fd = open(CAM_DEV_PATH, O_RDONLY);
  assert(fd >= 0);

  ESP_ERROR_CHECK(ioctl(fd, VIDIOC_QUERYCAP, &capability));
  print_video_device_info(&capability);

  context->cap_fd = fd;

  return 0;
}

static esp_err_t init_codec_video(camera_context* context) {
  int fd;
  const char* devpath = ENCODE_DEV_PATH;
  struct v4l2_capability capability;
  struct v4l2_ext_controls controls;
  struct v4l2_ext_control control[1];

  fd = open(devpath, O_RDONLY);
  assert(fd >= 0);

  ESP_ERROR_CHECK(ioctl(fd, VIDIOC_QUERYCAP, &capability));
  print_video_device_info(&capability);

  if (OUTPUT_FORMAT == V4L2_PIX_FMT_H264) {
    controls.ctrl_class = V4L2_CID_CODEC_CLASS;
    controls.count = 1;
    controls.controls = control;
    control[0].id = V4L2_CID_MPEG_VIDEO_H264_I_PERIOD;
    control[0].value = CONFIG_EXAMPLE_H264_I_PERIOD;
    if (ioctl(fd, VIDIOC_S_EXT_CTRLS, &controls) != 0) {
      ESP_LOGW(TAG, "failed to set H.264 intra frame period");
    }

    controls.ctrl_class = V4L2_CID_CODEC_CLASS;
    controls.count = 1;
    controls.controls = control;
    control[0].id = V4L2_CID_MPEG_VIDEO_BITRATE;
    control[0].value = CONFIG_EXAMPLE_H264_BITRATE;
    if (ioctl(fd, VIDIOC_S_EXT_CTRLS, &controls) != 0) {
      ESP_LOGW(TAG, "failed to set H.264 bitrate");
    }

    controls.ctrl_class = V4L2_CID_CODEC_CLASS;
    controls.count = 1;
    controls.controls = control;
    control[0].id = V4L2_CID_MPEG_VIDEO_H264_MIN_QP;
    control[0].value = CONFIG_EXAMPLE_H264_MIN_QP;
    if (ioctl(fd, VIDIOC_S_EXT_CTRLS, &controls) != 0) {
      ESP_LOGW(TAG, "failed to set H.264 minimum quality");
    }

    controls.ctrl_class = V4L2_CID_CODEC_CLASS;
    controls.count = 1;
    controls.controls = control;
    control[0].id = V4L2_CID_MPEG_VIDEO_H264_MAX_QP;
    control[0].value = CONFIG_EXAMPLE_H264_MAX_QP;
    if (ioctl(fd, VIDIOC_S_EXT_CTRLS, &controls) != 0) {
      ESP_LOGW(TAG, "failed to set H.264 maximum quality");
    }
  }

  // E (8324) h.264_video: id=9909ca is not supported
  // E (8328) esp_video: video->ops->set_ext_ctrl=106
  // W (8333) camera_driver: failed to set video b frames
  // E (8337) h.264_video: id=990a6b is not supported
  // E (8342) esp_video: video->ops->set_ext_ctrl=106
  // W (8346) camera_driver: failed to set h264 baseline

  // controls.ctrl_class = V4L2_CID_CODEC_CLASS;
  // controls.count = 1;
  // controls.controls = control;
  // control[0].id = V4L2_CID_MPEG_VIDEO_B_FRAMES;
  // control[0].value = 0;
  // if (ioctl(fd, VIDIOC_S_EXT_CTRLS, &controls) != 0) {
  //     ESP_LOGW(TAG, "failed to set video b frames");
  // }
  //
  // controls.ctrl_class = V4L2_CID_CODEC_CLASS;
  // controls.count = 1;
  // controls.controls = control;
  // control[0].id = V4L2_CID_MPEG_VIDEO_H264_PROFILE;
  // control[0].value = V4L2_MPEG_VIDEO_H264_PROFILE_BASELINE;
  // if (ioctl(fd, VIDIOC_S_EXT_CTRLS, &controls) != 0) {
  //     ESP_LOGW(TAG, "failed to set h264 baseline");
  // }

  context->format = OUTPUT_FORMAT;
  context->m2m_fd = fd;

  return 0;
}

esp_err_t video_start(int width, int height, camera_context* cb_ctx) {
  int type;
  struct v4l2_buffer buf;
  struct v4l2_format format;
  struct v4l2_requestbuffers req;
  uint32_t capture_fmt = 0;

  ESP_LOGW(TAG, "video start %dx%d", width, height);

  if (cb_ctx->format == V4L2_PIX_FMT_JPEG) {
    int fmt_index = 0;
    const uint32_t jpeg_input_formats[] = {V4L2_PIX_FMT_RGB565, V4L2_PIX_FMT_YUV422P, V4L2_PIX_FMT_RGB24,
                                           V4L2_PIX_FMT_GREY};
    int jpeg_input_formats_num = sizeof(jpeg_input_formats) / sizeof(jpeg_input_formats[0]);

    while (!capture_fmt) {
      struct v4l2_fmtdesc fmtdesc = {
          .index = fmt_index++,
          .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
      };

      if (ioctl(cb_ctx->cap_fd, VIDIOC_ENUM_FMT, &fmtdesc) != 0) {
        break;
      }

      for (int i = 0; i < jpeg_input_formats_num; i++) {
        if (jpeg_input_formats[i] == fmtdesc.pixelformat) {
          capture_fmt = jpeg_input_formats[i];
          ESP_LOGW(TAG, "capture fmt %d %d", i, capture_fmt);
          break;
        }
      }
    }

    if (!capture_fmt) {
      ESP_LOGI(TAG, "The camera sensor output pixel format is not supported by JPEG");
      return ESP_ERR_NOT_SUPPORTED;
    }
  } else {
    capture_fmt = V4L2_PIX_FMT_YUV420;
    ESP_LOGW(TAG, "capture fmt V4L2_PIX_FMT_YUV420");
  }

  /* Configure camera interface capture stream */

  memset(&format, 0, sizeof(format));
  format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  format.fmt.pix.width = width;
  format.fmt.pix.height = height;
  format.fmt.pix.pixelformat = capture_fmt;
  ESP_ERROR_CHECK(ioctl(cb_ctx->cap_fd, VIDIOC_S_FMT, &format));
  cb_ctx->cap_width = format.fmt.pix.width;
  cb_ctx->cap_height = format.fmt.pix.height;
  cb_ctx->cap_pixfmt = capture_fmt;
  cb_ctx->quarter_width = cb_ctx->cap_width / SELECTOR_SMALL_IMG_SCALE;
  cb_ctx->quarter_height = cb_ctx->cap_height / SELECTOR_SMALL_IMG_SCALE;
  if (cb_ctx->quarter_width == 0) cb_ctx->quarter_width = 1;
  if (cb_ctx->quarter_height == 0) cb_ctx->quarter_height = 1;
  cb_ctx->quarter_len = cb_ctx->quarter_width * cb_ctx->quarter_height;
  if (cb_ctx->quarter_img) {
    free(cb_ctx->quarter_img);
    cb_ctx->quarter_img = NULL;
  }
  cb_ctx->quarter_img = (uint8_t*)heap_caps_malloc(cb_ctx->quarter_len, MALLOC_CAP_8BIT);
  if (!cb_ctx->quarter_img) {
    ESP_LOGE(TAG, "alloc quarter image failed");
    return ESP_ERR_NO_MEM;
  }

  memset(&req, 0, sizeof(req));
  req.count = BUFFER_COUNT;
  req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  req.memory = V4L2_MEMORY_MMAP;
  ESP_ERROR_CHECK(ioctl(cb_ctx->cap_fd, VIDIOC_REQBUFS, &req));

  for (int i = 0; i < BUFFER_COUNT; i++) {
    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = i;
    ESP_ERROR_CHECK(ioctl(cb_ctx->cap_fd, VIDIOC_QUERYBUF, &buf));

    cb_ctx->cap_buffer[i] =
        (uint8_t*)mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, cb_ctx->cap_fd, buf.m.offset);
    assert(cb_ctx->cap_buffer[i]);

    ESP_ERROR_CHECK(ioctl(cb_ctx->cap_fd, VIDIOC_QBUF, &buf));
  }

  /* Configure codec output stream */

  memset(&format, 0, sizeof(format));
  format.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
  format.fmt.pix.width = width;
  format.fmt.pix.height = height;
  format.fmt.pix.pixelformat = capture_fmt;
  ESP_ERROR_CHECK(ioctl(cb_ctx->m2m_fd, VIDIOC_S_FMT, &format));

  memset(&req, 0, sizeof(req));
  req.count = 1;
  req.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
  req.memory = V4L2_MEMORY_USERPTR;
  ESP_ERROR_CHECK(ioctl(cb_ctx->m2m_fd, VIDIOC_REQBUFS, &req));

  /* Configure codec capture stream */

  memset(&format, 0, sizeof(format));
  format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  format.fmt.pix.width = width;
  format.fmt.pix.height = height;
  format.fmt.pix.pixelformat = cb_ctx->format;
  ESP_ERROR_CHECK(ioctl(cb_ctx->m2m_fd, VIDIOC_S_FMT, &format));

  memset(&req, 0, sizeof(req));
  req.count = 1;
  req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  req.memory = V4L2_MEMORY_MMAP;
  ESP_ERROR_CHECK(ioctl(cb_ctx->m2m_fd, VIDIOC_REQBUFS, &req));

  memset(&buf, 0, sizeof(buf));
  buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  buf.memory = V4L2_MEMORY_MMAP;
  buf.index = 0;
  ESP_ERROR_CHECK(ioctl(cb_ctx->m2m_fd, VIDIOC_QUERYBUF, &buf));

  cb_ctx->m2m_cap_buffer =
      (uint8_t*)mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, cb_ctx->m2m_fd, buf.m.offset);
  assert(cb_ctx->m2m_cap_buffer);

  ESP_ERROR_CHECK(ioctl(cb_ctx->m2m_fd, VIDIOC_QBUF, &buf));

  type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  ESP_ERROR_CHECK(ioctl(cb_ctx->m2m_fd, VIDIOC_STREAMON, &type));
  type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
  ESP_ERROR_CHECK(ioctl(cb_ctx->m2m_fd, VIDIOC_STREAMON, &type));

  type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  ESP_ERROR_CHECK(ioctl(cb_ctx->cap_fd, VIDIOC_STREAMON, &type));

  if (OUTPUT_FORMAT == V4L2_PIX_FMT_H264) {
    struct v4l2_ext_controls controls;
    struct v4l2_ext_control control[1];
    controls.ctrl_class = V4L2_CTRL_CLASS_MPEG;
    controls.count = 1;
    controls.controls = control;
    control[0].id = V4L2_CID_MPEG_VIDEO_BITRATE;
    if (ioctl(cb_ctx->m2m_fd, VIDIOC_G_EXT_CTRLS, &controls) == 0) {
      ESP_LOGI(TAG, "Current H.264 bitrate: %d bps", control[0].value);
    } else {
      ESP_LOGW(TAG, "Failed to get H.264 bitrate. The control may not be supported by your device.");
    }
  }
  return ESP_OK;
}

void video_stop(camera_context* cb_ctx) {
  int type;
  ESP_LOGD(TAG, "video stop");

  type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  ioctl(cb_ctx->cap_fd, VIDIOC_STREAMOFF, &type);

  type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
  ioctl(cb_ctx->m2m_fd, VIDIOC_STREAMOFF, &type);
  type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  ioctl(cb_ctx->m2m_fd, VIDIOC_STREAMOFF, &type);
  if (cb_ctx->quarter_img) {
    free(cb_ctx->quarter_img);
    cb_ctx->quarter_img = NULL;
  }
  cb_ctx->quarter_width = 0;
  cb_ctx->quarter_height = 0;
  cb_ctx->quarter_len = 0;
}


// [Camera Sensor]
//       ↓
//    (cap_fd)
//       ↓ DQBUF
// [Raw Frame in RAM]
//       ↓ QBUF (as input)
//    (m2m_fd)
//       ↓ processing
//       ↓ DQBUF
// [Processed Frame in RAM]
frame_buffer_t* video_fb_get(camera_context* cb_ctx) {
  struct v4l2_buffer cap_buf;
  struct v4l2_buffer m2m_out_buf;
  struct v4l2_buffer m2m_cap_buf;

  // dequeue one raw frame from the camera capture device
  memset(&cap_buf, 0, sizeof(cap_buf));
  cap_buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  cap_buf.memory = V4L2_MEMORY_MMAP;
  // VIDIOC_QBUF → “Here is a buffer, use it”
  // VIDIOC_DQBUF → “Give me a buffer that is done”
  ESP_ERROR_CHECK(ioctl(cb_ctx->cap_fd, VIDIOC_DQBUF, &cap_buf));

  // M2M device is a V4L2 device that:
  // takes image data from memory as input, processes it, and writes the result back to memory
  // queue that camera frame into the M2M device as input
  memset(&m2m_out_buf, 0, sizeof(m2m_out_buf));
  m2m_out_buf.index = 0;
  m2m_out_buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
  m2m_out_buf.memory = V4L2_MEMORY_USERPTR;
  m2m_out_buf.m.userptr = (unsigned long)cb_ctx->cap_buffer[cap_buf.index];
  m2m_out_buf.length = cap_buf.bytesused;
  ESP_ERROR_CHECK(ioctl(cb_ctx->m2m_fd, VIDIOC_QBUF, &m2m_out_buf));

  cb_ctx->raw_fb.buf = cb_ctx->cap_buffer[cap_buf.index];
  cb_ctx->raw_fb.len = cap_buf.bytesused;
  cb_ctx->raw_fb.width = cb_ctx->cap_width;
  cb_ctx->raw_fb.height = cb_ctx->cap_height;
  int64_t corner_t0_us = esp_timer_get_time();
  run_pixel_selector_quarter(cb_ctx, cb_ctx->raw_fb.buf, cb_ctx->raw_fb.width, cb_ctx->raw_fb.height);
  int64_t corner_t1_us = esp_timer_get_time();
  int32_t corner_cost_us = (int32_t)(corner_t1_us - corner_t0_us);
  frame_seq++;
  cb_ctx->raw_fb.frame_id = frame_seq;
  if ((frame_seq % SELECTOR_LOG_INTERVAL) == 0) {
    ESP_LOGI(TAG, "raw frame: %ux%u bytes=%u selected=%u quarter=%ux%u selector_time_ms=%" PRId32,
             (unsigned)cb_ctx->raw_fb.width,
             (unsigned)cb_ctx->raw_fb.height,
             (unsigned)cb_ctx->raw_fb.len,
             (unsigned)cb_ctx->corners_num,
             (unsigned)cb_ctx->quarter_width,
             (unsigned)cb_ctx->quarter_height,
             corner_cost_us / 1000);
    for (size_t i = 0; i < cb_ctx->corners_num && i < 8; ++i) {
      ESP_LOGI(TAG, "sel[%u] x=%u y=%u score=%" PRIu32,
               (unsigned)i,
               (unsigned)cb_ctx->corners[i].x,
               (unsigned)cb_ctx->corners[i].y,
               cb_ctx->corners[i].score);
    }
  }

  // dequeue the processed output from the M2M capture side
  // This waits until the M2M engine has produced output.
  memset(&m2m_cap_buf, 0, sizeof(m2m_cap_buf));
  m2m_cap_buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  m2m_cap_buf.memory = V4L2_MEMORY_MMAP;
  ESP_ERROR_CHECK(ioctl(cb_ctx->m2m_fd, VIDIOC_DQBUF, &m2m_cap_buf));

  // return the camera capture buffer back to the camera driver
  ESP_ERROR_CHECK(ioctl(cb_ctx->cap_fd, VIDIOC_QBUF, &cap_buf));
  // dequeue the M2M output-side input buffer completion
  ESP_ERROR_CHECK(ioctl(cb_ctx->m2m_fd, VIDIOC_DQBUF, &m2m_out_buf));

  // fill the returned frame descriptor
  cb_ctx->fb.buf = cb_ctx->m2m_cap_buffer;
  cb_ctx->fb.len = m2m_cap_buf.bytesused;
  cb_ctx->fb.frame_id = cb_ctx->raw_fb.frame_id;

  if (format_change) {
    struct v4l2_format format = {0};
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ESP_ERROR_CHECK(ioctl(cb_ctx->m2m_fd, VIDIOC_G_FMT, &format));
    cb_ctx->fb.width = format.fmt.pix.width;
    cb_ctx->fb.height = format.fmt.pix.height;
    format_change = false;
  }
#ifndef DIS_VIDEO_CODEC_TIMESTAMP
  int64_t us;
  us = esp_timer_get_time();
  cb_ctx->fb.timestamp.tv_sec = us / 1000000UL;
  ;
  cb_ctx->fb.timestamp.tv_usec = us % 1000000UL;
#endif

  return &cb_ctx->fb;
}

void video_after_take(const camera_context* cb_ctx) {
  struct v4l2_buffer m2m_cap_buf;

  // ESP_LOGD(TAG, "video return");

  m2m_cap_buf.index = 0;
  m2m_cap_buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  m2m_cap_buf.memory = V4L2_MEMORY_MMAP;
  ESP_ERROR_CHECK(ioctl(cb_ctx->m2m_fd, VIDIOC_QBUF, &m2m_cap_buf));
}

const frame_buffer_t* video_raw_fb_get(const camera_context* cb_ctx) {
  return cb_ctx ? &cb_ctx->raw_fb : NULL;
}

const corner_point_t* video_corner_points_get(const camera_context* cb_ctx, size_t* count) {
  if (!cb_ctx) {
    if (count) {
      *count = 0;
    }
    return NULL;
  }
  if (count) {
    *count = cb_ctx->corners_num;
  }
  return cb_ctx->corners;
}

const uint8_t* video_quarter_image_get(const camera_context* cb_ctx, size_t* width, size_t* height, size_t* len) {
  if (!cb_ctx) {
    if (width) *width = 0;
    if (height) *height = 0;
    if (len) *len = 0;
    return NULL;
  }
  if (width) *width = cb_ctx->quarter_width;
  if (height) *height = cb_ctx->quarter_height;
  if (len) *len = cb_ctx->quarter_len;
  return cb_ctx->quarter_img;
}

uint32_t video_frame_id_get(const camera_context* cb_ctx) {
  if (!cb_ctx) {
    return 0;
  }
  return cb_ctx->raw_fb.frame_id;
}

int video_dev_init(camera_context* context) {
  assert(context);

  ESP_ERROR_CHECK(esp_video_init(&cam_config));
  ESP_ERROR_CHECK(init_capture_video(context));
  ESP_ERROR_CHECK(init_codec_video(context));
  return ESP_OK;
}

void enumerate_camera_capabilities(int fd) {
  struct v4l2_fmtdesc fmtdesc;
  struct v4l2_frmsizeenum frmsize;
  struct v4l2_frmivalenum frmival;

  fmtdesc.index = 0;
  fmtdesc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  while (ioctl(fd, VIDIOC_ENUM_FMT, &fmtdesc) == 0) {
    printf("Format index[%lu]: %s (0x%08" PRIx32 ")\n", fmtdesc.index, (char*)fmtdesc.description, fmtdesc.pixelformat);

    frmsize.index = 0;
    frmsize.pixel_format = fmtdesc.pixelformat;

    while (ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &frmsize) == 0) {
      if (frmsize.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
        printf("  -> Resolution: %lux%lu\n", frmsize.discrete.width, frmsize.discrete.height);
      } else if (frmsize.type == V4L2_FRMSIZE_TYPE_STEPWISE || frmsize.type == V4L2_FRMSIZE_TYPE_CONTINUOUS) {
        printf("  -> Resolution Range: %lux%ld to %lux%lu\n", frmsize.stepwise.min_width, frmsize.stepwise.min_height,
               frmsize.stepwise.max_width, frmsize.stepwise.max_height);
      }

      frmival.index = 0;
      frmival.pixel_format = fmtdesc.pixelformat;
      frmival.width =
          (frmsize.type == V4L2_FRMSIZE_TYPE_DISCRETE) ? frmsize.discrete.width : frmsize.stepwise.min_width;
      frmival.height =
          (frmsize.type == V4L2_FRMSIZE_TYPE_DISCRETE) ? frmsize.discrete.height : frmsize.stepwise.min_height;

      while (ioctl(fd, VIDIOC_ENUM_FRAMEINTERVALS, &frmival) == 0) {
        if (frmival.type == V4L2_FRMIVAL_TYPE_DISCRETE) {
          printf("    -> Frame Interval: %lu/%lu fps\n", frmival.discrete.denominator, frmival.discrete.numerator);
        } else {
          printf("    -> %lux%ld to %lux%lu fps\n", frmival.stepwise.min.denominator, frmival.stepwise.min.numerator,
                 frmival.stepwise.max.denominator, frmival.stepwise.max.numerator);
        }
        frmival.index++;
      }
      frmsize.index++;
    }
    fmtdesc.index++;
    printf("\n");
  }
}
