/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: ESPRESSIF MIT
 */

#include "video_dev.h"
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

#define OUTPUT_FORMAT V4L2_PIX_FMT_H264
#define CAM_DEV_PATH ESP_VIDEO_MIPI_CSI_DEVICE_NAME
#define DIS_VIDEO_CODEC_TIMESTAMP 1

#if CONFIG_EXAMPLE_H264_MAX_QP <= CONFIG_EXAMPLE_H264_MIN_QP
#error "CONFIG_EXAMPLE_H264_MAX_QP should larger than CONFIG_EXAMPLE_H264_MIN_QP"
#endif

static const char* TAG = "camera_driver";

static bool format_change = true;

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
  }

  /* Configure camera interface capture stream */

  memset(&format, 0, sizeof(format));
  format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  format.fmt.pix.width = width;
  format.fmt.pix.height = height;
  format.fmt.pix.pixelformat = capture_fmt;
  ESP_ERROR_CHECK(ioctl(cb_ctx->cap_fd, VIDIOC_S_FMT, &format));

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

  struct v4l2_ext_controls controls;
  struct v4l2_ext_control control[1];
  controls.ctrl_class = V4L2_CTRL_CLASS_MPEG;  // 使用 MPEG 控制类
  controls.count = 1;
  controls.controls = control;

  control[0].id = V4L2_CID_MPEG_VIDEO_BITRATE;

  if (ioctl(cb_ctx->m2m_fd, VIDIOC_G_EXT_CTRLS, &controls) == 0) {
    ESP_LOGI(TAG, "Current H.264 bitrate: %d bps", control[0].value);
  } else {
    ESP_LOGW(TAG, "Failed to get H.264 bitrate. The control may not be supported by your device.");
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
}

frame_buffer_t* video_fb_get(camera_context* cb_ctx) {
  struct v4l2_buffer cap_buf;
  struct v4l2_buffer m2m_out_buf;
  struct v4l2_buffer m2m_cap_buf;

  memset(&cap_buf, 0, sizeof(cap_buf));
  cap_buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  cap_buf.memory = V4L2_MEMORY_MMAP;
  ESP_ERROR_CHECK(ioctl(cb_ctx->cap_fd, VIDIOC_DQBUF, &cap_buf));

  memset(&m2m_out_buf, 0, sizeof(m2m_out_buf));
  m2m_out_buf.index = 0;
  m2m_out_buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
  m2m_out_buf.memory = V4L2_MEMORY_USERPTR;
  m2m_out_buf.m.userptr = (unsigned long)cb_ctx->cap_buffer[cap_buf.index];
  m2m_out_buf.length = cap_buf.bytesused;
  ESP_ERROR_CHECK(ioctl(cb_ctx->m2m_fd, VIDIOC_QBUF, &m2m_out_buf));

  memset(&m2m_cap_buf, 0, sizeof(m2m_cap_buf));
  m2m_cap_buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  m2m_cap_buf.memory = V4L2_MEMORY_MMAP;
  ESP_ERROR_CHECK(ioctl(cb_ctx->m2m_fd, VIDIOC_DQBUF, &m2m_cap_buf));

  ESP_ERROR_CHECK(ioctl(cb_ctx->cap_fd, VIDIOC_QBUF, &cap_buf));
  ESP_ERROR_CHECK(ioctl(cb_ctx->m2m_fd, VIDIOC_DQBUF, &m2m_out_buf));

  cb_ctx->fb.buf = cb_ctx->m2m_cap_buffer;
  cb_ctx->fb.len = m2m_cap_buf.bytesused;

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

  ESP_LOGD(TAG, "video return");

  m2m_cap_buf.index = 0;
  m2m_cap_buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  m2m_cap_buf.memory = V4L2_MEMORY_MMAP;
  ESP_ERROR_CHECK(ioctl(cb_ctx->m2m_fd, VIDIOC_QBUF, &m2m_cap_buf));
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
