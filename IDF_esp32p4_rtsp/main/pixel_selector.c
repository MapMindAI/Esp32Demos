#include "pixel_selector.h"

#include "config.h"
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "linux/videodev2.h"

static const char* TAG = "pixel_selector";

static const float kDirs[16][2] = {
    {0.0f, 1.0f},       {0.3827f, 0.9239f},  {0.1951f, 0.9808f},   {0.9239f, 0.3827f},
    {0.7071f, 0.7071f}, {0.3827f, -0.9239f}, {0.8315f, 0.5556f},   {0.8315f, -0.5556f},
    {0.5556f, -0.8315f},{0.9808f, 0.1951f},  {0.9239f, -0.3827f},  {0.7071f, -0.7071f},
    {0.5556f, 0.8315f}, {0.9808f, -0.1951f}, {1.0f, 0.0f},         {0.1951f, -0.9808f},
};

static inline uint8_t rgb_to_gray_u8(int r, int g, int b) {
  int y = (77 * r + 150 * g + 29 * b) >> 8;
  if (y < 0) y = 0;
  if (y > 255) y = 255;
  return (uint8_t)y;
}

static inline uint8_t raw_gray_at(const uint8_t* raw, uint32_t cap_pixfmt, int x, int y, int w, int h) {
  if (x < 0) x = 0;
  if (y < 0) y = 0;
  if (x >= w) x = w - 1;
  if (y >= h) y = h - 1;

  switch (cap_pixfmt) {
    case V4L2_PIX_FMT_GREY:
      return raw[y * w + x];
    case V4L2_PIX_FMT_YUV420:
    case V4L2_PIX_FMT_NV12:
    case V4L2_PIX_FMT_NV21:
    case V4L2_PIX_FMT_YUV422P:
      return raw[y * w + x];
    case V4L2_PIX_FMT_YUYV: {
      int pair_x = x & ~1;
      int idx = (y * w + pair_x) * 2 + (x & 1 ? 2 : 0);
      return raw[idx];
    }
    case V4L2_PIX_FMT_UYVY: {
      int pair_x = x & ~1;
      int idx = (y * w + pair_x) * 2 + (x & 1 ? 3 : 1);
      return raw[idx];
    }
    case V4L2_PIX_FMT_RGB24: {
      int idx = (y * w + x) * 3;
      return rgb_to_gray_u8(raw[idx + 0], raw[idx + 1], raw[idx + 2]);
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
      return raw[y * w + x];
  }
}

typedef struct {
  TaskHandle_t task_handle;
  TaskHandle_t caller_task;
  volatile bool started;
  bool stop;

  const uint8_t* raw_img;
  uint32_t cap_pixfmt;
  int src_w;
  int src_h;
  uint8_t* small_img;
  int qw;
  int qh;
  int yb_start;
  int yb_end;
  int w32;
  int h32;
  int blk;
  int potential;
  int grad_hist_add;
  float threshold_scale;
  float best_val_min;
  uint32_t local_seed;

  int* th_hist;
  float* th_smooth;
  int th_w32;
  int th_h32;

  corner_point_t out_points[MAX_CORNER_POINTS];
  size_t out_count;
} pipeline_worker_t;

static pipeline_worker_t* g_workers[2];
static pipeline_worker_t* g_inline_worker;
static bool g_workers_ready;
static bool g_init_done;
static bool g_workers_ready_logged;

static void pipeline_worker_task(void* arg);

static pipeline_worker_t* alloc_worker_ctx(const char* tag_name) {
  pipeline_worker_t* w =
      (pipeline_worker_t*)heap_caps_malloc(sizeof(pipeline_worker_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (w) {
    memset(w, 0, sizeof(*w));
    return w;
  }
  w = (pipeline_worker_t*)heap_caps_malloc(sizeof(pipeline_worker_t), MALLOC_CAP_8BIT);
  if (!w) {
    ESP_LOGE(TAG, "alloc %s worker failed (%u bytes, free_heap=%u)", tag_name, (unsigned)sizeof(pipeline_worker_t),
             (unsigned)esp_get_free_heap_size());
    return NULL;
  }
  memset(w, 0, sizeof(*w));
  ESP_LOGW(TAG, "alloc %s worker fallback to internal RAM", tag_name);
  return w;
}

static void clear_task_notifications(void) {
  while (ulTaskNotifyTake(pdTRUE, 0) > 0) {
  }
}

static void selector_workers_try_start(void) {
#if SELECTOR_USE_DUAL_CORE && !CONFIG_FREERTOS_UNICORE
  if (g_workers_ready) {
    return;
  }

  bool created = false;
  for (int i = 0; i < 2; ++i) {
    pipeline_worker_t* w = g_workers[i];
    if (!w) {
      w = alloc_worker_ctx(i == 0 ? "top" : "bottom");
      if (!w) {
        continue;
      }
      g_workers[i] = w;
    }
    if (w->task_handle) {
      continue;
    }
    BaseType_t r = xTaskCreatePinnedToCore(pipeline_worker_task, i == 0 ? "sel_p0" : "sel_p1", 4096, w, 5,
                                           &w->task_handle, i);
    if (r == pdPASS) {
      created = true;
    } else {
      ESP_LOGW(TAG, "selector worker %d create failed (free_heap=%u)", i, (unsigned)esp_get_free_heap_size());
      w->task_handle = NULL;
    }
  }

  TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(200);
  while (xTaskGetTickCount() < deadline) {
    if (g_workers[0] && g_workers[1] && g_workers[0]->task_handle && g_workers[1]->task_handle &&
        g_workers[0]->started && g_workers[1]->started) {
      break;
    }
    vTaskDelay(pdMS_TO_TICKS(1));
  }

  g_workers_ready = (g_workers[0] && g_workers[1] && g_workers[0]->task_handle && g_workers[1]->task_handle &&
                     g_workers[0]->started && g_workers[1]->started);
  if (g_workers_ready) {
    if (!g_workers_ready_logged) {
      ESP_LOGI(TAG, "selector_workers_init ready (dual full pipelines)");
      g_workers_ready_logged = true;
    }
  } else if (created) {
    ESP_LOGW(TAG, "selector workers not ready yet (w0=%p started=%d w1=%p started=%d)",
             g_workers[0] ? (void*)g_workers[0]->task_handle : NULL,
             (g_workers[0] && g_workers[0]->started) ? 1 : 0,
             g_workers[1] ? (void*)g_workers[1]->task_handle : NULL,
             (g_workers[1] && g_workers[1]->started) ? 1 : 0);
  }
#endif
}

static esp_err_t ensure_worker_threshold_buffers(pipeline_worker_t* w, int w32, int h32) {
  if (!w || w32 <= 0 || h32 <= 0) {
    return ESP_ERR_INVALID_ARG;
  }
  if (w->th_hist && w->th_smooth && w->th_w32 == w32 && w->th_h32 == h32) {
    return ESP_OK;
  }
  if (w->th_hist) {
    free(w->th_hist);
    w->th_hist = NULL;
  }
  if (w->th_smooth) {
    free(w->th_smooth);
    w->th_smooth = NULL;
  }

  size_t elems = (size_t)w32 * (size_t)h32;
  w->th_hist = (int*)heap_caps_malloc(elems * sizeof(int), MALLOC_CAP_8BIT);
  w->th_smooth = (float*)heap_caps_malloc(elems * sizeof(float), MALLOC_CAP_8BIT);
  if (!w->th_hist || !w->th_smooth) {
    if (w->th_hist) {
      free(w->th_hist);
      w->th_hist = NULL;
    }
    if (w->th_smooth) {
      free(w->th_smooth);
      w->th_smooth = NULL;
    }
    w->th_w32 = 0;
    w->th_h32 = 0;
    return ESP_ERR_NO_MEM;
  }

  w->th_w32 = w32;
  w->th_h32 = h32;
  return ESP_OK;
}

static void convert_small_gray_region(pipeline_worker_t* w) {
  const int scale = SELECTOR_SMALL_IMG_SCALE;
  int conv_margin = w->blk + w->potential + 2;
  int y0 = w->yb_start - conv_margin;
  int y1 = w->yb_end + conv_margin;
  if (y0 < 0) y0 = 0;
  if (y1 > w->qh) y1 = w->qh;

  for (int y = y0; y < y1; ++y) {
    int sy = y * scale;
    for (int x = 0; x < w->qw; ++x) {
      int sx = x * scale;
      uint32_t ssum = 0;
      for (int by = 0; by < scale; ++by) {
        for (int bx = 0; bx < scale; ++bx) {
          ssum += raw_gray_at(w->raw_img, w->cap_pixfmt, sx + bx, sy + by, w->src_w, w->src_h);
        }
      }
      w->small_img[y * w->qw + x] = (uint8_t)(ssum / (uint32_t)(scale * scale));
    }
  }
}

static void compute_threshold_map_region(pipeline_worker_t* w) {
  const uint8_t* small_img = w->small_img;
  const int qw = w->qw;
  const int qh = w->qh;
  const int blk = w->blk;
  const int w32 = w->w32;
  const int h32 = w->h32;

  int by0 = w->yb_start / blk;
  int by1 = (w->yb_end + blk - 1) / blk;
  if (by0 < 0) by0 = 0;
  if (by1 > h32) by1 = h32;
  if (by1 <= by0) {
    return;
  }

  int hist_by0 = by0 > 0 ? by0 - 1 : 0;
  int hist_by1 = by1 < h32 ? by1 + 1 : h32;

  for (int by = hist_by0; by < hist_by1; ++by) {
    for (int bx = 0; bx < w32; ++bx) {
      int hist[51] = {0};
      int xs = bx * blk;
      int ys = by * blk;
      int xe = xs + blk > qw ? qw : xs + blk;
      int ye = ys + blk > qh ? qh : ys + blk;
      int cnt = 0;
      for (int y = ys; y < ye; ++y) {
        for (int x = xs; x < xe; ++x) {
          int gg = 0;
          if (x > 0 && x < qw - 1 && y > 0 && y < qh - 1) {
            int idx = y * qw + x;
            int dx = (int)small_img[idx + 1] - (int)small_img[idx - 1];
            int dy = (int)small_img[idx + qw] - (int)small_img[idx - qw];
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
      w->th_hist[bx + by * w32] = q + w->grad_hist_add;
    }
  }

  for (int by = by0; by < by1; ++by) {
    for (int bx = 0; bx < w32; ++bx) {
      int sum = 0;
      int num = 0;
      for (int oy = -1; oy <= 1; ++oy) {
        for (int ox = -1; ox <= 1; ++ox) {
          int nx = bx + ox;
          int ny = by + oy;
          if (nx < 0 || nx >= w32 || ny < 0 || ny >= h32) {
            continue;
          }
          sum += w->th_hist[nx + ny * w32];
          num++;
        }
      }
      float a = (float)sum / (float)num;
      w->th_smooth[bx + by * w32] = a * a;
    }
  }
}

static size_t select_points_region(const pipeline_worker_t* w, corner_point_t* out, size_t max_out) {
  size_t out_count = 0;
  int y_end = w->yb_end;
  if (y_end > w->qh - w->potential) {
    y_end = w->qh - w->potential;
  }

  for (int yb = w->yb_start; yb < y_end; yb += w->potential) {
    for (int xb = 1; xb < w->qw - w->potential; xb += w->potential) {
      if (out_count >= max_out) {
        return out_count;
      }

      int best_idx = -1;
      float best_val = 0.0f;
      const float* dir = kDirs[(out_count + w->local_seed) & 15];
      for (int dy0 = 0; dy0 < w->potential; ++dy0) {
        for (int dx0 = 0; dx0 < w->potential; ++dx0) {
          int x = xb + dx0;
          int y = yb + dy0;
          int idx = y * w->qw + x;
          float th = w->th_smooth[(x >> 5) + (y >> 5) * w->w32] * w->threshold_scale;
          int dx = (int)w->small_img[idx + 1] - (int)w->small_img[idx - 1];
          int dy = (int)w->small_img[idx + w->qw] - (int)w->small_img[idx - w->qw];
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

      if (best_idx >= 0 && best_val >= w->best_val_min) {
        out[out_count].x = (uint16_t)(best_idx % w->qw);
        out[out_count].y = (uint16_t)(best_idx / w->qw);
        out[out_count].score = (uint32_t)best_val;
        out_count++;
      }
    }
  }

  return out_count;
}

static void pipeline_worker_task(void* arg) {
  pipeline_worker_t* w = (pipeline_worker_t*)arg;
  w->started = true;
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    if (w->stop) {
      break;
    }

    convert_small_gray_region(w);
    compute_threshold_map_region(w);
    w->out_count = select_points_region(w, w->out_points, MAX_CORNER_POINTS);
    if (w->caller_task) {
      xTaskNotifyGive(w->caller_task);
    }
  }
  vTaskDelete(NULL);
}

static size_t run_pipeline_inline(pipeline_worker_t* w, corner_point_t* out_points, size_t max_points) {
  if (ensure_worker_threshold_buffers(w, w->w32, w->h32) != ESP_OK) {
    return 0;
  }
  convert_small_gray_region(w);
  compute_threshold_map_region(w);
  return select_points_region(w, out_points, max_points);
}

esp_err_t pixel_selector_init(void) {
  if (g_init_done) {
    selector_workers_try_start();
    return ESP_OK;
  }
  g_init_done = true;

#if SELECTOR_USE_DUAL_CORE && !CONFIG_FREERTOS_UNICORE
  selector_workers_try_start();
  if (!g_workers_ready) {
    ESP_LOGW(TAG, "selector workers init pending/fail, fallback to single-core pipeline");
  }
#else
  g_workers_ready = false;
#endif

  return ESP_OK;
}

int pixel_selector_run(const uint8_t* raw,
                       int src_w,
                       int src_h,
                       uint32_t cap_pixfmt,
                       uint8_t* small_img,
                       int small_w,
                       int small_h,
                       corner_point_t* out_points,
                       size_t max_points,
                       uint32_t seed) {
  if (!raw || !small_img || !out_points || src_w <= 0 || src_h <= 0 || small_w < 4 || small_h < 4 || max_points == 0) {
    return 0;
  }
  (void)pixel_selector_init();

  const int blk = SELECTOR_BLOCK_SIZE;
  const int w32 = (small_w + blk - 1) / blk;
  const int h32 = (small_h + blk - 1) / blk;
  const int potential = SELECTOR_POTENTIAL;

  pipeline_worker_t* top = g_workers[0];
  pipeline_worker_t* bot = g_workers[1];
  int split_y = small_h / 2;
  if (split_y < 2) {
    split_y = 2;
  }
  if (split_y > small_h - 2) {
    split_y = small_h - 2;
  }

#if SELECTOR_USE_DUAL_CORE && !CONFIG_FREERTOS_UNICORE
  if (!g_workers_ready) {
    selector_workers_try_start();
    top = g_workers[0];
    bot = g_workers[1];
  }
  if (g_workers_ready && top && bot && top->task_handle && bot->task_handle) {
    if (ensure_worker_threshold_buffers(top, w32, h32) != ESP_OK || ensure_worker_threshold_buffers(bot, w32, h32) != ESP_OK) {
      g_workers_ready = false;
      ESP_LOGW(TAG, "selector worker buffers alloc failed, fallback to single-core pipeline");
    } else {
      TaskHandle_t caller = xTaskGetCurrentTaskHandle();

      top->caller_task = caller;
      top->raw_img = raw;
      top->cap_pixfmt = cap_pixfmt;
      top->src_w = src_w;
      top->src_h = src_h;
      top->small_img = small_img;
      top->qw = small_w;
      top->qh = small_h;
      top->yb_start = 1;
      top->yb_end = split_y;
      top->w32 = w32;
      top->h32 = h32;
      top->blk = blk;
      top->potential = potential;
      top->grad_hist_add = SELECTOR_GRAD_HIST_ADD;
      top->threshold_scale = SELECTOR_THRESHOLD_SCALE;
      top->best_val_min = SELECTOR_BEST_VAL_MIN;
      top->local_seed = seed;

      bot->caller_task = caller;
      bot->raw_img = raw;
      bot->cap_pixfmt = cap_pixfmt;
      bot->src_w = src_w;
      bot->src_h = src_h;
      bot->small_img = small_img;
      bot->qw = small_w;
      bot->qh = small_h;
      bot->yb_start = split_y;
      bot->yb_end = small_h - potential;
      bot->w32 = w32;
      bot->h32 = h32;
      bot->blk = blk;
      bot->potential = potential;
      bot->grad_hist_add = SELECTOR_GRAD_HIST_ADD;
      bot->threshold_scale = SELECTOR_THRESHOLD_SCALE;
      bot->best_val_min = SELECTOR_BEST_VAL_MIN;
      bot->local_seed = seed + 3;

      clear_task_notifications();
      xTaskNotifyGive(top->task_handle);
      xTaskNotifyGive(bot->task_handle);
      if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(300)) == 0 || ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(300)) == 0) {
        ESP_LOGW(TAG, "selector workers timeout, fallback to single-core this frame");
        g_workers_ready = false;
      } else {
        size_t total = 0;
        size_t c0 = top->out_count < max_points ? top->out_count : max_points;
        memcpy(out_points, top->out_points, c0 * sizeof(corner_point_t));
        total += c0;

        size_t remain = max_points - total;
        size_t c1 = bot->out_count < remain ? bot->out_count : remain;
        memcpy(out_points + total, bot->out_points, c1 * sizeof(corner_point_t));
        total += c1;
        return (int)total;
      }
    }
  }
#endif

  if (!g_inline_worker) {
    g_inline_worker = alloc_worker_ctx("inline");
    if (!g_inline_worker) {
      return 0;
    }
  }
  g_inline_worker->raw_img = raw;
  g_inline_worker->cap_pixfmt = cap_pixfmt;
  g_inline_worker->src_w = src_w;
  g_inline_worker->src_h = src_h;
  g_inline_worker->small_img = small_img;
  g_inline_worker->qw = small_w;
  g_inline_worker->qh = small_h;
  g_inline_worker->yb_start = 1;
  g_inline_worker->yb_end = small_h - potential;
  g_inline_worker->w32 = w32;
  g_inline_worker->h32 = h32;
  g_inline_worker->blk = blk;
  g_inline_worker->potential = potential;
  g_inline_worker->grad_hist_add = SELECTOR_GRAD_HIST_ADD;
  g_inline_worker->threshold_scale = SELECTOR_THRESHOLD_SCALE;
  g_inline_worker->best_val_min = SELECTOR_BEST_VAL_MIN;
  g_inline_worker->local_seed = seed;

  return (int)run_pipeline_inline(g_inline_worker, out_points, max_points);
}
