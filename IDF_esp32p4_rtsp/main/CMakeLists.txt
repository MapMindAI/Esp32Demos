idf_component_register(
SRCS
  "main.c"
  "ws_stream_service.c"
  "video_dev.c"
  "pixel_selector.c"
  "wifi_station.c"

INCLUDE_DIRS "."
REQUIRES
  esp_driver_jpeg
  nvs_flash
  esp_wifi
  esp_netif
  esp_event
  esp_http_server
  mdns)
