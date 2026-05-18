/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: ESPRESSIF MIT
 */

#include "video_dev.h"
#include "config.h"
#include "pixel_selector.h"
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/param.h>
#include <unistd.h>
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_video_device.h"
#include "esp_video_init.h"
#include "driver/jpeg_decode.h"
#include "linux/videodev2.h"

#include <esp_heap_caps.h>
#include <esp_system.h>
#include <inttypes.h>

#define OUTPUT_FORMAT V4L2_PIX_FMT_JPEG
#define DIS_VIDEO_CODEC_TIMESTAMP 1

#if CONFIG_EXAMPLE_H264_MAX_QP <= CONFIG_EXAMPLE_H264_MIN_QP
#error "CONFIG_EXAMPLE_H264_MAX_QP should larger than CONFIG_EXAMPLE_H264_MIN_QP"
#endif

static const char* TAG = "camera_driver";

static bool format_change = true;
static uint32_t frame_seq;

#if CONFIG_ESP_VIDEO_ENABLE_USB_UVC_VIDEO_DEVICE
static const esp_video_init_usb_uvc_config_t usb_uvc_config = {
    .uvc = {
        .uvc_dev_num = 1,
        .task_stack = 6144,
        .task_priority = 13,
        .task_affinity = -1,
    },
    .usb = {
        .init_usb_host_lib = true,
        .peripheral_map = 0x00,
        .task_stack = 6144,
        .task_priority = 14,
        .task_affinity = -1,
    },
};

static const esp_video_init_config_t cam_config = {
    .usb_uvc = &usb_uvc_config,
};
#else
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
#endif

static void run_pixel_selector_quarter(camera_context* cb_ctx, const uint8_t* y_plane, size_t width, size_t height) {
  cb_ctx->corners_num = 0;
  if (!y_plane || !cb_ctx->quarter_img || cb_ctx->quarter_width < 4 || cb_ctx->quarter_height < 4) {
    return;
  }

  cb_ctx->corners_num = (size_t)pixel_selector_run(y_plane,
                                                    (int)width,
                                                    (int)height,
                                                    cb_ctx->cap_pixfmt,
                                                    cb_ctx->quarter_img,
                                                    (int)cb_ctx->quarter_width,
                                                    (int)cb_ctx->quarter_height,
                                                    cb_ctx->corners,
                                                    MAX_CORNER_POINTS,
                                                    frame_seq);
}

static esp_err_t ensure_jpeg_decoder(camera_context* cb_ctx) {
  if (cb_ctx->jpeg_dec) {
    return ESP_OK;
  }
  jpeg_decoder_handle_t decoder = NULL;
  jpeg_decode_engine_cfg_t cfg = {
      .intr_priority = 0,
      .timeout_ms = 40,
  };
  esp_err_t ret = jpeg_new_decoder_engine(&cfg, &decoder);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "jpeg_new_decoder_engine failed: %s", esp_err_to_name(ret));
    return ret;
  }
  cb_ctx->jpeg_dec = (void*)decoder;
  return ESP_OK;
}

static esp_err_t decode_jpeg_to_gray(camera_context* cb_ctx, const uint8_t* jpeg_buf, size_t jpeg_len) {
  if (!cb_ctx || !jpeg_buf || jpeg_len == 0) {
    return ESP_ERR_INVALID_ARG;
  }

  esp_err_t ret = ensure_jpeg_decoder(cb_ctx);
  if (ret != ESP_OK) {
    return ret;
  }

  if (!cb_ctx->jpeg_in_buf || cb_ctx->jpeg_in_buf_size < jpeg_len) {
    jpeg_decode_memory_alloc_cfg_t in_mem_cfg = {
        .buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER,
    };
    size_t allocated_in = 0;
    uint8_t* new_in = (uint8_t*)jpeg_alloc_decoder_mem(jpeg_len, &in_mem_cfg, &allocated_in);
    if (!new_in || allocated_in < jpeg_len) {
      ESP_LOGE(TAG, "jpeg input buffer alloc failed req=%u alloc=%u", (unsigned)jpeg_len, (unsigned)allocated_in);
      if (new_in) {
        free(new_in);
      }
      return ESP_ERR_NO_MEM;
    }
    if (cb_ctx->jpeg_in_buf) {
      free(cb_ctx->jpeg_in_buf);
    }
    cb_ctx->jpeg_in_buf = new_in;
    cb_ctx->jpeg_in_buf_size = allocated_in;
  }
  memcpy(cb_ctx->jpeg_in_buf, jpeg_buf, jpeg_len);

  jpeg_decode_picture_info_t info = {0};
  ret = jpeg_decoder_get_info(cb_ctx->jpeg_in_buf, (uint32_t)jpeg_len, &info);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "jpeg_decoder_get_info failed: %s", esp_err_to_name(ret));
    return ret;
  }

  size_t aligned_w = (info.width + 15U) & ~15U;
  size_t aligned_h = (info.height + 15U) & ~15U;
  size_t required = aligned_w * aligned_h * 2U;
  if (required == 0) {
    return ESP_ERR_INVALID_SIZE;
  }

  if (!cb_ctx->jpeg_gray_buf || cb_ctx->jpeg_gray_buf_size < required) {
    jpeg_decode_memory_alloc_cfg_t out_mem_cfg = {
        .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
    };
    size_t allocated = 0;
    uint8_t* new_buf = (uint8_t*)jpeg_alloc_decoder_mem(required, &out_mem_cfg, &allocated);
    if (!new_buf || allocated < required) {
      ESP_LOGE(TAG, "jpeg rgb565 buffer alloc failed req=%u alloc=%u", (unsigned)required, (unsigned)allocated);
      if (new_buf) {
        free(new_buf);
      }
      return ESP_ERR_NO_MEM;
    }
    if (cb_ctx->jpeg_gray_buf) {
      free(cb_ctx->jpeg_gray_buf);
    }
    cb_ctx->jpeg_gray_buf = new_buf;
    cb_ctx->jpeg_gray_buf_size = allocated;
  }

  jpeg_decode_cfg_t dec_cfg = {
      .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
      .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_RGB,
      .conv_std = JPEG_YUV_RGB_CONV_STD_BT601,
  };
  uint32_t out_size = 0;
  ret = jpeg_decoder_process((jpeg_decoder_handle_t)cb_ctx->jpeg_dec, &dec_cfg, cb_ctx->jpeg_in_buf, (uint32_t)jpeg_len,
                             cb_ctx->jpeg_gray_buf, (uint32_t)cb_ctx->jpeg_gray_buf_size, &out_size);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "jpeg_decoder_process failed: %s", esp_err_to_name(ret));
    return ret;
  }

  cb_ctx->jpeg_gray_width = info.width;
  cb_ctx->jpeg_gray_height = info.height;
  return ESP_OK;
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

  fd = open(CAM_DEV_PATH, O_RDWR);
  if (fd < 0) {
    ESP_LOGE(TAG, "open camera device failed: %s errno=%d (%s)", CAM_DEV_PATH, errno, strerror(errno));
    return (errno == ENOENT) ? ESP_ERR_NOT_FOUND : ESP_FAIL;
  }
  ESP_LOGI(TAG, "open camera device: %s", CAM_DEV_PATH);

  if (ioctl(fd, VIDIOC_QUERYCAP, &capability) != 0) {
    ESP_LOGE(TAG, "VIDIOC_QUERYCAP failed for camera %s: errno=%d (%s)", CAM_DEV_PATH, errno, strerror(errno));
    close(fd);
    return ESP_FAIL;
  }
  print_video_device_info(&capability);

  context->cap_fd = fd;

  return ESP_OK;
}

static esp_err_t init_codec_video(camera_context* context) {
  int fd;
  const char* devpath = ENCODE_DEV_PATH;
  struct v4l2_capability capability;
  struct v4l2_ext_controls controls;
  struct v4l2_ext_control control[1];

  fd = open(devpath, O_RDWR);
  if (fd < 0) {
    ESP_LOGE(TAG, "open encoder device failed: %s errno=%d (%s)", devpath, errno, strerror(errno));
    return (errno == ENOENT) ? ESP_ERR_NOT_FOUND : ESP_FAIL;
  }

  if (ioctl(fd, VIDIOC_QUERYCAP, &capability) != 0) {
    ESP_LOGE(TAG, "VIDIOC_QUERYCAP failed for encoder %s: errno=%d (%s)", devpath, errno, strerror(errno));
    close(fd);
    return ESP_FAIL;
  }
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

  return ESP_OK;
}

esp_err_t video_start(int width, int height, camera_context* cb_ctx) {
  int type;
  struct v4l2_buffer buf;
  struct v4l2_format format;
  struct v4l2_requestbuffers req;
  uint32_t capture_fmt = 0;
  cb_ctx->direct_jpeg = false;

  ESP_LOGW(TAG, "video start %dx%d", width, height);

  enumerate_camera_capabilities(cb_ctx->cap_fd);

  // TODO: use V4L2_PIX_FMT_YUV422P by default
  if (cb_ctx->format == V4L2_PIX_FMT_JPEG) {
    int fmt_index = 0;
#if CONFIG_ESP_VIDEO_ENABLE_USB_UVC_VIDEO_DEVICE
    const uint32_t jpeg_input_formats[] = {
        V4L2_PIX_FMT_YUYV,
        V4L2_PIX_FMT_UYVY,
        V4L2_PIX_FMT_YUV420,
        V4L2_PIX_FMT_NV12,
        V4L2_PIX_FMT_NV21,
        V4L2_PIX_FMT_YUV422P,
        V4L2_PIX_FMT_GREY,
    };
#else
    const uint32_t jpeg_input_formats[] = {
        V4L2_PIX_FMT_JPEG,
        V4L2_PIX_FMT_YUYV,
        V4L2_PIX_FMT_UYVY,
        V4L2_PIX_FMT_YUV420,
        V4L2_PIX_FMT_NV12,
        V4L2_PIX_FMT_NV21,
        V4L2_PIX_FMT_RGB565,
        V4L2_PIX_FMT_YUV422P,
        V4L2_PIX_FMT_RGB24,
        V4L2_PIX_FMT_GREY,
    };
#endif
    size_t jpeg_input_formats_num = sizeof(jpeg_input_formats) / sizeof(jpeg_input_formats[0]);

    while (!capture_fmt) {
      struct v4l2_fmtdesc fmtdesc = {
          .index = fmt_index++,
          .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
      };

      if (ioctl(cb_ctx->cap_fd, VIDIOC_ENUM_FMT, &fmtdesc) != 0) {
        break;
      }

      for (size_t i = 0; i < jpeg_input_formats_num; i++) {
        ESP_LOGW(TAG, "check capture fmt %u (0x%08" PRIx32 ")-(0x%08" PRIx32 ")", (unsigned)i, fmtdesc.pixelformat, jpeg_input_formats[i]);
        if (jpeg_input_formats[i] == fmtdesc.pixelformat) {
          capture_fmt = jpeg_input_formats[i];
          ESP_LOGW(TAG, "capture fmt prefer[%u] fourcc=0x%08" PRIx32, (unsigned)i, capture_fmt);
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
#if CONFIG_ESP_VIDEO_ENABLE_USB_UVC_VIDEO_DEVICE
  {
    struct v4l2_streamparm streamparm = {0};
    streamparm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    streamparm.parm.capture.timeperframe.numerator = 1;
    streamparm.parm.capture.timeperframe.denominator = 15;  // request 15 FPS to reduce USB isoc pressure
    if (ioctl(cb_ctx->cap_fd, VIDIOC_S_PARM, &streamparm) != 0) {
      ESP_LOGW(TAG, "VIDIOC_S_PARM(15fps) failed errno=%d (%s)", errno, strerror(errno));
    } else {
      ESP_LOGI(TAG, "capture fps set request: %u/%u",
               (unsigned)streamparm.parm.capture.timeperframe.denominator,
               (unsigned)streamparm.parm.capture.timeperframe.numerator);
    }
  }
#endif
#if CONFIG_ESP_VIDEO_ENABLE_USB_UVC_VIDEO_DEVICE
  // For USB path we only need quarter grayscale for selector; bypass codec regardless of capture format.
  cb_ctx->direct_jpeg = true;
#else
  cb_ctx->direct_jpeg = (capture_fmt == V4L2_PIX_FMT_JPEG);
#endif
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

  if (!cb_ctx->direct_jpeg) {
    /* Configure codec output stream */
    memset(&format, 0, sizeof(format));
    format.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    format.fmt.pix.width = width;
    format.fmt.pix.height = height;
    format.fmt.pix.pixelformat = capture_fmt;
    if (ioctl(cb_ctx->m2m_fd, VIDIOC_S_FMT, &format) != 0) {
      ESP_LOGE(TAG, "jpeg set output fmt failed fourcc=0x%08" PRIx32 " errno=%d (%s)", capture_fmt, errno,
               strerror(errno));
      return ESP_FAIL;
    }

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
    if (ioctl(cb_ctx->m2m_fd, VIDIOC_S_FMT, &format) != 0) {
      ESP_LOGE(TAG, "jpeg set capture fmt failed fourcc=0x%08" PRIx32 " errno=%d (%s)", cb_ctx->format, errno,
               strerror(errno));
      return ESP_FAIL;
    }

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
  } else {
    ESP_LOGI(TAG, "camera provides JPEG directly, bypass codec m2m");
  }

  type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  ESP_ERROR_CHECK(ioctl(cb_ctx->cap_fd, VIDIOC_STREAMON, &type));

  if (!cb_ctx->direct_jpeg && OUTPUT_FORMAT == V4L2_PIX_FMT_H264) {
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

  if (!cb_ctx->direct_jpeg) {
    type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    ioctl(cb_ctx->m2m_fd, VIDIOC_STREAMOFF, &type);
    type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(cb_ctx->m2m_fd, VIDIOC_STREAMOFF, &type);
  }
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

  cb_ctx->raw_fb.buf = cb_ctx->cap_buffer[cap_buf.index];
  cb_ctx->raw_fb.len = cap_buf.bytesused;
  cb_ctx->raw_fb.width = cb_ctx->cap_width;
  cb_ctx->raw_fb.height = cb_ctx->cap_height;
  frame_seq++;
  cb_ctx->raw_fb.frame_id = frame_seq;

  if (cb_ctx->direct_jpeg) {
    const uint8_t* selector_src = cb_ctx->raw_fb.buf;
    size_t selector_w = cb_ctx->raw_fb.width;
    size_t selector_h = cb_ctx->raw_fb.height;
    uint32_t selector_fmt = cb_ctx->cap_pixfmt;

    if (cb_ctx->cap_pixfmt == V4L2_PIX_FMT_JPEG) {
      esp_err_t ret = decode_jpeg_to_gray(cb_ctx, cb_ctx->raw_fb.buf, cb_ctx->raw_fb.len);
      if (ret == ESP_OK && cb_ctx->jpeg_gray_buf && cb_ctx->jpeg_gray_width > 0 && cb_ctx->jpeg_gray_height > 0) {
        selector_src = cb_ctx->jpeg_gray_buf;
        selector_w = cb_ctx->jpeg_gray_width;
        selector_h = cb_ctx->jpeg_gray_height;
        selector_fmt = V4L2_PIX_FMT_RGB565;
      } else {
        ESP_LOGW(TAG, "jpeg->rgb565 decode failed, fallback to empty quarter");
        cb_ctx->corners_num = 0;
        if (cb_ctx->quarter_img && cb_ctx->quarter_len > 0) {
          memset(cb_ctx->quarter_img, 0, cb_ctx->quarter_len);
        }
        selector_src = NULL;
      }
    }

    if (selector_src && selector_w > 0 && selector_h > 0) {
      int64_t corner_t0_us = esp_timer_get_time();
      uint32_t old_fmt = cb_ctx->cap_pixfmt;
      cb_ctx->cap_pixfmt = selector_fmt;
      run_pixel_selector_quarter(cb_ctx, selector_src, selector_w, selector_h);
      cb_ctx->cap_pixfmt = old_fmt;
      int64_t corner_t1_us = esp_timer_get_time();
      int32_t corner_cost_us = (int32_t)(corner_t1_us - corner_t0_us);
      if ((frame_seq % SELECTOR_LOG_INTERVAL) == 0) {
        ESP_LOGI(TAG, "raw frame(direct): %ux%u bytes=%u selected=%u quarter=%ux%u selector_time_ms=%" PRId32,
                 (unsigned)selector_w,
                 (unsigned)selector_h,
                 (unsigned)cb_ctx->raw_fb.len,
                 (unsigned)cb_ctx->corners_num,
                 (unsigned)cb_ctx->quarter_width,
                 (unsigned)cb_ctx->quarter_height,
                 corner_cost_us / 1000);
      }
    }
    cb_ctx->fb.buf = cb_ctx->cap_buffer[cap_buf.index];
    cb_ctx->fb.len = cap_buf.bytesused;
    cb_ctx->fb.frame_id = cb_ctx->raw_fb.frame_id;
    cb_ctx->fb.width = cb_ctx->cap_width;
    cb_ctx->fb.height = cb_ctx->cap_height;
    return &cb_ctx->fb;
  }

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

  int64_t corner_t0_us = esp_timer_get_time();
  run_pixel_selector_quarter(cb_ctx, cb_ctx->raw_fb.buf, cb_ctx->raw_fb.width, cb_ctx->raw_fb.height);
  int64_t corner_t1_us = esp_timer_get_time();
  int32_t corner_cost_us = (int32_t)(corner_t1_us - corner_t0_us);
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
  if (cb_ctx->direct_jpeg) {
    struct v4l2_buffer cap_buf;
    memset(&cap_buf, 0, sizeof(cap_buf));
    cap_buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    cap_buf.memory = V4L2_MEMORY_MMAP;
    cap_buf.index = 0;
    // Recover index from current raw buffer pointer
    for (int i = 0; i < BUFFER_COUNT; ++i) {
      if (cb_ctx->cap_buffer[i] == cb_ctx->raw_fb.buf) {
        cap_buf.index = i;
        break;
      }
    }
    ESP_ERROR_CHECK(ioctl(cb_ctx->cap_fd, VIDIOC_QBUF, &cap_buf));
    return;
  }

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

esp_err_t video_dev_init(camera_context* context) {
  assert(context);
  esp_err_t ret;

  ret = esp_video_init(&cam_config);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "esp_video_init failed: %s", esp_err_to_name(ret));
    return ret;
  }

  ret = init_capture_video(context);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "init_capture_video failed: %s", esp_err_to_name(ret));
    return ret;
  }

#if CONFIG_ESP_VIDEO_ENABLE_USB_UVC_VIDEO_DEVICE
  // USB selector-only pipeline: do not open/use JPEG codec device.
  context->format = OUTPUT_FORMAT;
  context->m2m_fd = -1;
  ESP_LOGI(TAG, "USB mode: skip codec init, use raw YUV capture for selector");
#else
  ret = init_codec_video(context);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "init_codec_video failed: %s", esp_err_to_name(ret));
    return ret;
  }
#endif

  ret = pixel_selector_init();
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "pixel_selector_init failed: %s", esp_err_to_name(ret));
    return ret;
  }

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
