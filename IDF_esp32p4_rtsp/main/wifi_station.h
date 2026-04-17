#pragma once


#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"

#define EXAMPLE_MDNS_HOST_NAME "esp32p4-rtsp"

void wifi_init_sta(void);
