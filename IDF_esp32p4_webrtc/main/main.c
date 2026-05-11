/* Door Bell Demo

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <esp_event.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_wifi.h>
#include <nvs_flash.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/param.h>
#include "common.h"
#include "esp_capture.h"
#include "esp_console.h"
#include "esp_cpu.h"
#include "esp_timer.h"
#include "esp_webrtc.h"
#include "media_lib_adapter.h"
#include "media_lib_netif.h"
#include "media_lib_os.h"
#include "robot_canbus.h"
#include "settings.h"
#include "mqtt_bridge.h"
#include "webrtc_utils_time.h"

static const char* TAG = "Webrtc_Test";
static atomic_llong s_last_heartbeat_ms = 0;
static bool s_webrtc_state_last = false;
static int64_t s_last_status_pub_ms = 0;
#define WEBRTC_HEARTBEAT_TIMEOUT_MS (15000)
#define WEBRTC_STATUS_PUB_PERIOD_MS (5000)

#define RUN_ASYNC(name, body)       \
  void run_async##name(void* arg) { \
    body;                           \
    media_lib_thread_destroy(NULL); \
  }                                 \
  media_lib_thread_create_from_scheduler(NULL, #name, run_async##name, NULL);

static int start_cli(int argc, char** argv) {
  start_webrtc(NULL);
  return 0;
}

static int stop_cli(int argc, char** argv) {
  RUN_ASYNC(leave, { stop_webrtc(); });
  return 0;
}

static int close_data_ch_cli(int argc, char** argv) {
  close_data_channel(argc > 1 ? atoi(argv[1]) : 0);
  return 0;
}

static int cmd_cli(int argc, char** argv) {
  send_cmd(argc > 1 ? argv[1] : "ring");
  return 0;
}

static int assert_cli(int argc, char** argv) {
  *(int*)0 = 0;
  return 0;
}

static int sys_cli(int argc, char** argv) {
  sys_state_show();
  return 0;
}

static int wifi_cli(int argc, char** argv) {
  if (argc < 1) {
    return -1;
  }
  char* ssid = argv[1];
  char* password = argc > 2 ? argv[2] : NULL;
  return network_connect_wifi(ssid, password);
}

static int capture_to_player_cli(int argc, char** argv) { return test_capture_to_player(); }

static int measure_cli(int argc, char** argv) {
  void measure_enable(bool enable);
  void show_measure(void);
  measure_enable(true);
  media_lib_thread_sleep(1500);
  measure_enable(false);
  return 0;
}

static int init_console() {
  esp_console_repl_t* repl = NULL;
  esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
  repl_config.prompt = "esp>";
  repl_config.task_stack_size = 10 * 1024;
  repl_config.task_priority = 22;
  repl_config.max_cmdline_length = 1024;
  // install console REPL environment
#if CONFIG_ESP_CONSOLE_UART
  esp_console_dev_uart_config_t uart_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_console_new_repl_uart(&uart_config, &repl_config, &repl));
#elif CONFIG_ESP_CONSOLE_USB_CDC
  esp_console_dev_usb_cdc_config_t cdc_config = ESP_CONSOLE_DEV_CDC_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_console_new_repl_usb_cdc(&cdc_config, &repl_config, &repl));
#elif CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
  esp_console_dev_usb_serial_jtag_config_t usbjtag_config = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&usbjtag_config, &repl_config, &repl));
#endif

  esp_console_cmd_t cmds[] = {
      {
          .command = "start",
          .help = "Start signaling and webrtc.\r\n",
          .func = start_cli,
      },
      {
          .command = "stop",
          .help = "Stop signaling and webrtc.\n",
          .func = stop_cli,
      },
      {
          .command = "cmd",
          .help = "Send command (ring etc)\n",
          .func = cmd_cli,
      },
      {
          .command = "close",
          .help = "Close data channel\n",
          .func = close_data_ch_cli,
      },
      {
          .command = "i",
          .help = "Show system status\r\n",
          .func = sys_cli,
      },
      {
          .command = "assert",
          .help = "Assert system\r\n",
          .func = assert_cli,
      },
      {
          .command = "rec2play",
          .help = "Play capture content\n",
          .func = capture_to_player_cli,
      },
      {
          .command = "wifi",
          .help = "wifi ssid psw\r\n",
          .func = wifi_cli,
      },
      {
          .command = "m",
          .help = "measure system loading\r\n",
          .func = measure_cli,
      },
  };
  for (int i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmds[i]));
  }
  ESP_ERROR_CHECK(esp_console_start_repl(repl));
  return 0;
}

static void thread_scheduler(const char* thread_name, media_lib_thread_cfg_t* schedule_cfg) {
  if (strcmp(thread_name, "venc_0") == 0) {
    // For H264 may need huge stack if use hardware encoder can set it to small value
    schedule_cfg->priority = 10;
#if CONFIG_IDF_TARGET_ESP32S3
    schedule_cfg->stack_size = 20 * 1024;
#endif
  }
#ifdef WEBRTC_SUPPORT_OPUS
  else if (strcmp(thread_name, "aenc_0") == 0) {
    // For OPUS encoder it need huge stack, when use G711 can set it to small value
    schedule_cfg->stack_size = 40 * 1024;
    schedule_cfg->priority = 10;
    schedule_cfg->core_id = 1;
  }
#endif
  else if (strcmp(thread_name, "AUD_SRC") == 0) {
    schedule_cfg->priority = 15;
  } else if (strcmp(thread_name, "pc_task") == 0) {
    schedule_cfg->stack_size = 25 * 1024;
    schedule_cfg->priority = 18;
    schedule_cfg->core_id = 1;
  }
  if (strcmp(thread_name, "start") == 0) {
    schedule_cfg->stack_size = 6 * 1024;
  }
}

static void capture_scheduler(const char* name, esp_capture_thread_schedule_cfg_t* schedule_cfg) {
  media_lib_thread_cfg_t cfg = {
      .stack_size = schedule_cfg->stack_size,
      .priority = schedule_cfg->priority,
      .core_id = schedule_cfg->core_id,
  };
  schedule_cfg->stack_in_ext = true;
  thread_scheduler(name, &cfg);
  schedule_cfg->stack_size = cfg.stack_size;
  schedule_cfg->priority = cfg.priority;
  schedule_cfg->core_id = cfg.core_id;
}

static char* get_network_ip(void) {
  media_lib_ipv4_info_t ip_info;
  media_lib_netif_get_ipv4_info(MEDIA_LIB_NET_TYPE_STA, &ip_info);
  return media_lib_ipv4_ntoa(&ip_info.ip);
}

void sctp_show_details(bool enable);

static void mqtt_control_cmd_handler(const char *cmd, void *ctx) {
  if (!cmd) {
    return;
  }
  int64_t now_ms = esp_timer_get_time() / 1000;
  if (strcmp(cmd, "OPEN_WEBRTC") == 0) {
    atomic_store(&s_last_heartbeat_ms, now_ms);
    if (!webrtc_is_running()) {
      ESP_LOGI(TAG, "MQTT command: OPEN_WEBRTC");
      start_webrtc(NULL);
    }
  } else if (strcmp(cmd, "CLOSE_WEBRTC") == 0) {
    ESP_LOGI(TAG, "MQTT command: CLOSE_WEBRTC");
    stop_webrtc();
  } else if (strcmp(cmd, "HEARTBEAT") == 0) {
    atomic_store(&s_last_heartbeat_ms, now_ms);
    if (!webrtc_is_running()) {
      ESP_LOGI(TAG, "MQTT command: HEARTBEAT -> OPEN_WEBRTC");
      start_webrtc(NULL);
    }
  } else {
    // Forward user control commands (e.g. Up/Down/Left/Right/Stop) to WebRTC app logic.
    send_cmd((char *)cmd);
  }
}

static int get_wifi_rssi_dbm(void) {
  wifi_ap_record_t ap_info = {0};
  if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
    return ap_info.rssi;
  }
  return -127;
}

static void publish_webrtc_status(const char *state, const char *reason) {
  char payload[220];
  bool running = webrtc_is_running();
  bool connected = webrtc_is_connected();
  int wifi_rssi = get_wifi_rssi_dbm();
  if (reason && reason[0]) {
    snprintf(payload, sizeof(payload),
             "{\"webrtc\":\"%s\",\"running\":%s,\"connected\":%s,\"wifi_rssi\":%d,\"reason\":\"%s\"}",
             state, running ? "true" : "false", connected ? "true" : "false", wifi_rssi, reason);
  } else {
    snprintf(payload, sizeof(payload),
             "{\"webrtc\":\"%s\",\"running\":%s,\"connected\":%s,\"wifi_rssi\":%d}",
             state, running ? "true" : "false", connected ? "true" : "false", wifi_rssi);
  }
  mqtt_bridge_publish_json("status", payload);
}

static void mqtt_control_loop(void) {
  int64_t now_ms = esp_timer_get_time() / 1000;
  bool connected = webrtc_is_connected();
  if (connected != s_webrtc_state_last) {
    s_webrtc_state_last = connected;
    if (connected) {
      publish_webrtc_status("ready", NULL);
    } else {
      publish_webrtc_status("stopped", NULL);
    }
  }
  if (mqtt_bridge_is_connected() && now_ms > s_last_status_pub_ms + WEBRTC_STATUS_PUB_PERIOD_MS) {
    s_last_status_pub_ms = now_ms;
    publish_webrtc_status(connected ? "ready" : (webrtc_is_running() ? "connecting" : "idle"), NULL);
  }
  int64_t hb_ms = atomic_load(&s_last_heartbeat_ms);
  if (webrtc_is_running() && hb_ms > 0 && now_ms - hb_ms > WEBRTC_HEARTBEAT_TIMEOUT_MS) {
    ESP_LOGW(TAG, "Heartbeat timeout, stopping WebRTC to save power");
    stop_webrtc();
    publish_webrtc_status("stopped", "heartbeat_timeout");
    atomic_store(&s_last_heartbeat_ms, 0);
  }
}

static int network_event_handler(bool connected) {
  if (connected) {
    mqtt_bridge_set_cmd_handler(mqtt_control_cmd_handler, NULL);
    mqtt_bridge_start();
    publish_webrtc_status("idle", NULL);
    ESP_LOGI(TAG, "MQTT control ready at %s", get_network_ip());
    // sctp_show_details(true);
  } else {
    mqtt_bridge_stop();
    stop_webrtc();
    atomic_store(&s_last_heartbeat_ms, 0);
    s_last_status_pub_ms = 0;
  }
  return 0;
}

#define CANBUS_RX_PIN GPIO_NUM_26
#define CANBUS_TX_PIN GPIO_NUM_27

void app_main(void) {
  esp_log_level_set("*", ESP_LOG_INFO);
  InitializeCanbus(CANBUS_RX_PIN, CANBUS_TX_PIN);

  media_lib_add_default_adapter();
  esp_capture_set_thread_scheduler(capture_scheduler);
  media_lib_thread_set_schedule_cb(thread_scheduler);
  init_board();
  media_sys_buildup();
  init_console();
  network_init(WIFI_SSID, WIFI_PASSWORD, network_event_handler);
  while (1) {
    // int64_t boottime_ms = esp_timer_get_time();
    // UpdateLed(boottime_ms);
    media_lib_thread_sleep(2000);
    query_webrtc();
    mqtt_control_loop();
    print_memory_info();
  }
}
