#ifndef PIXEL_SELECTOR_H
#define PIXEL_SELECTOR_H

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "video_dev.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t pixel_selector_init(void);

int pixel_selector_run(const uint8_t* raw,
                       int src_w,
                       int src_h,
                       uint32_t cap_pixfmt,
                       uint8_t* small_img,
                       int small_w,
                       int small_h,
                       corner_point_t* out_points,
                       size_t max_points,
                       uint32_t seed);

#ifdef __cplusplus
}
#endif

#endif
