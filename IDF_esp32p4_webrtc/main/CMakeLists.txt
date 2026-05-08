
set(srcs
  "webrtc.c"
  "main.c"
  "board.c"
  "media_sys.c"
  "webrtc_http_server.c"
  "mqtt_bridge.c"
)

idf_component_register(SRCS ${srcs}
                       EMBED_TXTFILES "embed_files/ring.aac" "embed_files/open.aac" "embed_files/join.aac"
                       "embed_files/webrtc_test.html"
                       "certs/servercert.pem" "certs/prvtkey.pem"
                       INCLUDE_DIRS "."
                       REQUIRES
                       common
                       esp_capture
                       esp_webrtc
                       codec_board
                       av_render
                       media_lib_sal
                       webrtc_utils
                       robot_canbus
                       esp_wifi
                       esp_event
                       nvs_flash
                       esp_http_server
                       esp_https_server
                       esp_timer
                       console
                       json
                       PRIV_REQUIRES mqtt)
