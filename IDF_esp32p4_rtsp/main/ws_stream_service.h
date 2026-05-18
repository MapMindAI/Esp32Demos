#ifndef _WS_STREAM_SERVICE_H
#define _WS_STREAM_SERVICE_H

#include "esp_err.h"
#include "video_dev.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t ws_stream_service_start(camera_context* camera_context);
esp_err_t ws_stream_service_stop(void);

#ifdef __cplusplus
}
#endif

#endif
