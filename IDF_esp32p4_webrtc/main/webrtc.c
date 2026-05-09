/* DoorBell WebRTC application code

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include "esp_random.h"
#include "esp_timer.h"
#include "common.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_peer.h"
#include "esp_peer_default.h"
#include "esp_webrtc.h"
#include "esp_webrtc_defaults.h"
#include "media_lib_os.h"
#include "robot_canbus.h"
#if CONFIG_DOORBELL_SIGNALING_LOCAL_HTTP
#include "webrtc_http_server.h"
#endif

#define TAG "DOOR_BELL"

// Customized commands
#define DOOR_BELL_OPEN_DOOR_CMD "OPEN_DOOR"
#define DOOR_BELL_DOOR_OPENED_CMD "DOOR_OPENED"
#define DOOR_BELL_RING_CMD "RING"
#define DOOR_BELL_CALL_ACCEPTED_CMD "ACCEPT_CALL"
#define DOOR_BELL_CALL_DENIED_CMD "DENY_CALL"
#define DOOR_BELL_ENABLE_RTP_TRANSFORMER "ENABLE_RTP_TRANSFORMER"
#define DOOR_BELL_DISABLE_RTP_TRANSFORMER "DISABLE_RTP_TRANSFORMER"

#define SAME_STR(a, b) (strncmp(a, b, sizeof(b) - 1) == 0)
#define SEND_CMD(webrtc, cmd) \
  esp_webrtc_send_custom_data(webrtc, ESP_WEBRTC_CUSTOM_DATA_VIA_SIGNALING, (uint8_t*)cmd, strlen(cmd))
#define ELEMS(arr) sizeof(arr) / sizeof(arr[0])
#define RTP_HEADER_SIZE 12

typedef enum {
  DOOR_BELL_STATE_NONE,
  DOOR_BELL_STATE_RINGING,
  DOOR_BELL_STATE_CONNECTING,
  DOOR_BELL_STATE_CONNECTED,
} door_bell_state_t;

typedef enum {
  DOOR_BELL_TONE_RING,
  DOOR_BELL_TONE_OPEN_DOOR,
  DOOR_BELL_TONE_JOIN_SUCCESS,
} door_bell_tone_type_t;

typedef struct {
  const uint8_t* start;
  const uint8_t* end;
  int duration;
} door_bell_tone_data_t;

typedef struct {
  esp_peer_data_channel_info_t info;
  int send_count;
  int recv_count;
  bool used;
} user_data_ch_t;

static esp_webrtc_handle_t webrtc;
static door_bell_state_t door_bell_state;
static bool monitor_key;
static user_data_ch_t user_ch[2];
static bool data_running = false;
static bool janus_retry_running = false;
static bool janus_retry_stop = false;
static bool webrtc_connected = false;
int start_webrtc(char* url);

#if CONFIG_DOORBELL_SIGNALING_JANUS
#define JANUS_RETRY_INTERVAL_MS 5000
#endif

extern const uint8_t ring_music_start[] asm("_binary_ring_aac_start");
extern const uint8_t ring_music_end[] asm("_binary_ring_aac_end");
extern const uint8_t open_music_start[] asm("_binary_open_aac_start");
extern const uint8_t open_music_end[] asm("_binary_open_aac_end");
extern const uint8_t join_music_start[] asm("_binary_join_aac_start");
extern const uint8_t join_music_end[] asm("_binary_join_aac_end");

// RTP Transformer test context
typedef struct {
  uint32_t frame_count;
  uint32_t error_count;
} rtp_transformer_ctx_t;

static rtp_transformer_ctx_t sender_transformer_ctx;
static rtp_transformer_ctx_t receiver_transformer_ctx;
static bool rtp_transformer_enabled = false;
static uint32_t imu_sync_send_count = 0;
static uint32_t imu_sync_skip_count = 0;
static uint32_t imu_sync_fail_count = 0;
static uint32_t imu_last_pts = UINT32_MAX;
static uint32_t imu_seq = 0;
static bool imu_data_channel_connected = false;
static bool imu_data_channel_create_requested = false;

static float randf_range(float min_v, float max_v) {
  uint32_t r = esp_random();
  float t = (float)(r & 0xFFFF) / 65535.0f;
  return min_v + (max_v - min_v) * t;
}

__attribute__((weak)) bool board_read_imu_sample(float* ax, float* ay, float* az, float* gx, float* gy, float* gz) {
  // Default test generator when no real IMU driver is linked.
  if (!ax || !ay || !az || !gx || !gy || !gz) {
    return false;
  }
  *ax = randf_range(-0.2f, 0.2f);
  *ay = randf_range(-0.2f, 0.2f);
  *az = randf_range(0.85f, 1.15f);
  *gx = randf_range(-3.0f, 3.0f);
  *gy = randf_range(-3.0f, 3.0f);
  *gz = randf_range(-3.0f, 3.0f);
  return true;
}

static user_data_ch_t* get_first_open_data_channel(void) {
  for (int i = 0; i < ELEMS(user_ch); i++) {
    if (user_ch[i].used) {
      return &user_ch[i];
    }
  }
  return NULL;
}

static void reset_imu_sync_state(void) {
  imu_sync_send_count = 0;
  imu_sync_skip_count = 0;
  imu_sync_fail_count = 0;
  imu_last_pts = UINT32_MAX;
  imu_data_channel_connected = false;
  imu_data_channel_create_requested = false;
}

static void request_imu_data_channel(const char* reason) {
  if (!webrtc) {
    return;
  }
  esp_peer_handle_t peer_handle = NULL;
  int ret = esp_webrtc_get_peer_connection(webrtc, &peer_handle);
  if (ret != ESP_PEER_ERR_NONE || !peer_handle) {
    ESP_LOGW(TAG, "IMU sync unable to request data channel (%s), peer_handle unavailable ret=%d", reason ? reason : "-",
             ret);
    return;
  }

  esp_peer_data_channel_cfg_t ch_cfg = {
      .type = ESP_PEER_DATA_CHANNEL_RELIABLE,
      .ordered = true,
      .label = "imu",
  };
  ret = esp_peer_create_data_channel(peer_handle, &ch_cfg);
  if (ret == ESP_PEER_ERR_NONE) {
    imu_data_channel_create_requested = true;
    ESP_LOGI(TAG, "IMU sync requested local data channel label=%s reason=%s", ch_cfg.label, reason ? reason : "-");
  } else if (ret != ESP_PEER_ERR_WRONG_STATE) {
    ESP_LOGW(TAG, "IMU sync request data channel failed ret=%d reason=%s", ret, reason ? reason : "-");
  }
}

static int send_imu_synced_with_video(esp_peer_video_frame_t* frame, void* ctx) {
  (void)ctx;
  if (!frame || !webrtc || !webrtc_connected) {
    return 0;
  }
  // Send once per unique video PTS.
  if (frame->pts == imu_last_pts) {
    return 0;
  }
  imu_last_pts = frame->pts;

  user_data_ch_t* data_ch = get_first_open_data_channel();
  bool fallback_stream = false;
  uint16_t stream_id = 0;
  if (!data_ch) {
    fallback_stream = true;
    imu_sync_skip_count++;
    if (imu_sync_skip_count == 1 || (imu_sync_skip_count % 120) == 0) {
      ESP_LOGW(TAG,
               "IMU sync no tracked data channel, fallback stream_id=0 skipped=%" PRIu32
               " dc_connected=%d create_requested=%d",
               imu_sync_skip_count, imu_data_channel_connected, imu_data_channel_create_requested);
    }
    if (!imu_data_channel_create_requested || (imu_sync_skip_count % 240) == 0) {
      request_imu_data_channel("video_send_fallback");
    }
  } else {
    stream_id = data_ch->info.stream_id;
  }

  float ax = 0.0f, ay = 0.0f, az = 1.0f;
  float gx = 0.0f, gy = 0.0f, gz = 0.0f;
  int valid = board_read_imu_sample(&ax, &ay, &az, &gx, &gy, &gz) ? 1 : 0;
  uint64_t ts_us = esp_timer_get_time();
  uint32_t ts_ms = (uint32_t)(ts_us / 1000);

  char payload[256];
  int n = snprintf(payload, sizeof(payload),
                   "{\"type\":\"imu\",\"seq\":%" PRIu32 ",\"video_pts\":%" PRIu32 ",\"ts_ms\":%" PRIu32
                   ",\"ts_us\":%" PRIu64
                   ",\"valid\":%d,\"acc\":[%.4f,%.4f,%.4f],\"gyro\":[%.4f,%.4f,%.4f]}",
                   imu_seq++, frame->pts, ts_ms, ts_us, valid, ax, ay, az, gx, gy, gz);
  if (n <= 0 || n >= (int)sizeof(payload)) {
    return 0;
  }
  esp_peer_handle_t peer_handle = NULL;
  int ret = esp_webrtc_get_peer_connection(webrtc, &peer_handle);
  if (ret != 0 || !peer_handle) {
    return 0;
  }
  esp_peer_data_frame_t data_frame = {
      .type = ESP_PEER_DATA_CHANNEL_STRING,
      .stream_id = stream_id,
      .data = (uint8_t*)payload,
      .size = n,
  };
  ret = esp_peer_send_data(peer_handle, &data_frame);
  if (ret == 0) {
    imu_sync_send_count++;
    if (imu_sync_send_count % 120 == 0) {
      ESP_LOGI(TAG, "IMU sync sent count=%" PRIu32 " last_video_pts=%" PRIu32, imu_sync_send_count, frame->pts);
    }
  } else if (ret != ESP_PEER_ERR_WOULD_BLOCK && ret != ESP_PEER_ERR_WRONG_STATE) {
    imu_sync_fail_count++;
    if (imu_sync_fail_count == 1 || (imu_sync_fail_count % 120) == 0) {
      ESP_LOGW(TAG, "IMU sync send failed ret=%d stream_id=%u fallback=%d fail_count=%" PRIu32, ret, stream_id,
               fallback_stream, imu_sync_fail_count);
    }
  }
  return 0;
}

static void cleanup_webrtc_handle(void) {
  if (webrtc) {
    monitor_key = false;
    esp_webrtc_handle_t handle = webrtc;
    webrtc = NULL;
    ESP_LOGI(TAG, "Start to close webrtc %p", handle);
    esp_webrtc_close(handle);
    while (data_running) {
      media_lib_thread_sleep(10);
    }
  }
}

#if CONFIG_DOORBELL_SIGNALING_JANUS
static void janus_retry_task(void* arg) {
  janus_retry_running = true;
  while (!janus_retry_stop) {
    if (!network_is_connected()) {
      break;
    }
    if (webrtc == NULL) {
      ESP_LOGW(TAG, "Janus unavailable, retrying WebRTC start...");
      if (start_webrtc(NULL) == 0) {
        ESP_LOGI(TAG, "WebRTC reconnect succeeded");
        break;
      }
    }
    media_lib_thread_sleep(JANUS_RETRY_INTERVAL_MS);
  }
  janus_retry_running = false;
  media_lib_thread_destroy(NULL);
}

static void schedule_janus_retry(void) {
  if (janus_retry_stop || janus_retry_running || !network_is_connected()) {
    return;
  }
  media_lib_thread_handle_t retry_thread;
  media_lib_thread_create_from_scheduler(&retry_thread, "janus_retry", janus_retry_task, NULL);
}
#endif

static int play_tone(door_bell_tone_type_t type) {
  door_bell_tone_data_t tone_data[] = {
      {ring_music_start, ring_music_end, 4000},
      {open_music_start, open_music_end, 0},
      {join_music_start, join_music_end, 0},
  };
  if (type >= sizeof(tone_data) / sizeof(tone_data[0])) {
    return 0;
  }
  return play_music(tone_data[type].start, (int)(tone_data[type].end - tone_data[type].start),
                    tone_data[type].duration);
}

int play_tone_int(int t) { return play_tone((door_bell_tone_type_t)t); }

static void door_bell_change_state(door_bell_state_t state) {
  door_bell_state = state;
  if (state == DOOR_BELL_STATE_CONNECTING || state == DOOR_BELL_STATE_NONE) {
    stop_music();
  }
}

uint32_t current_velocity_ = 125;
uint32_t servo_velocity_ = 5;

static bool is_cmd(const char* cmd, const char* exact, const char* legacy) {
  if (exact && strcmp(cmd, exact) == 0) {
    return true;
  }
  return legacy && strcmp(cmd, legacy) == 0;
}

static bool ProcessServoCommand(const char* cmd) {
  if (is_cmd(cmd, "SERVO_UP", "HeadUp")) {
    AddMessageToSend(18, servo_velocity_);
    return true;
  } else if (is_cmd(cmd, "SERVO_DOWN", "HeadDown")) {
    AddMessageToSend(12, servo_velocity_);
    return true;
  } else if (is_cmd(cmd, "SERVO_LEFT", "HeadLeft")) {
    AddMessageToSend(14, servo_velocity_);
    return true;
  } else if (is_cmd(cmd, "SERVO_RIGHT", "HeadRight")) {
    AddMessageToSend(16, servo_velocity_);
    return true;
  } else if (is_cmd(cmd, "SERVO_STOP", NULL)) {
    AddMessageToSend(15, 0);
    return true;
  }
  return false;
}

static bool ProcessServoConfigCommand(const char* cmd) {
  const char* prefix = "SERVO_STEP:";
  const size_t prefix_len = 11;
  if (strncmp(cmd, prefix, prefix_len) != 0) {
    return false;
  }
  long step = strtol(cmd + prefix_len, NULL, 10);
  if (step < 0) {
    step = 0;
  } else if (step > 20) {
    step = 20;
  }
  servo_velocity_ = (uint32_t)step;
  ESP_LOGI(TAG, "Servo step updated: %" PRIu32, servo_velocity_);
  return true;
}

static bool ProcessMoveCommand(const char* cmd) {
  if (is_cmd(cmd, "ROBOT_UP", "Up")) {
    AddMessageToSend(8, current_velocity_);
    return true;
  } else if (is_cmd(cmd, "ROBOT_DOWN", "Down")) {
    AddMessageToSend(2, current_velocity_);
    return true;
  } else if (is_cmd(cmd, "ROBOT_LEFT", "Left")) {
    AddMessageToSend(4, current_velocity_);
    return true;
  } else if (is_cmd(cmd, "ROBOT_RIGHT", "Right")) {
    AddMessageToSend(6, current_velocity_);
    return true;
  } else if (is_cmd(cmd, "ROBOT_STOP", "Stop")) {
    AddMessageToSend(5, 0);
    return true;
  }
  return false;
}

static int door_bell_on_cmd(esp_webrtc_custom_data_via_t via, uint8_t* data, int size, void* ctx) {
  // Only handle signaling message
  if (via != ESP_WEBRTC_CUSTOM_DATA_VIA_SIGNALING) {
    return 0;
  }
  if (size == 0 || webrtc == NULL) {
    return 0;
  }
  ESP_LOGI(TAG, "Receive command %.*s", size, (char*)data);
  const char* cmd = (const char*)data;

  if (ProcessServoConfigCommand(cmd)) {
    return 0;
  }

  if (ProcessServoCommand(cmd)) {
    return 0;
  }

  if (ProcessMoveCommand(cmd)) {
    return 0;
  }

  if (SAME_STR(cmd, DOOR_BELL_OPEN_DOOR_CMD)) {
    // Reply with door OPENED
    SEND_CMD(webrtc, DOOR_BELL_DOOR_OPENED_CMD);
    // Only play tome when connection not build up
    if (door_bell_state < DOOR_BELL_STATE_CONNECTING) {
      play_tone(DOOR_BELL_TONE_OPEN_DOOR);
    }
  } else if (SAME_STR(cmd, DOOR_BELL_CALL_ACCEPTED_CMD)) {
    door_bell_change_state(DOOR_BELL_STATE_CONNECTING);
    esp_webrtc_enable_peer_connection(webrtc, true);
  } else if (SAME_STR(cmd, DOOR_BELL_CALL_DENIED_CMD)) {
    esp_webrtc_enable_peer_connection(webrtc, false);
    door_bell_change_state(DOOR_BELL_STATE_NONE);
  } else if (SAME_STR(cmd, DOOR_BELL_ENABLE_RTP_TRANSFORMER)) {
    if (!rtp_transformer_enabled) {
      rtp_transformer_enabled = true;
      ESP_LOGI(TAG, "RTP transformer enabled via command");
    }
  } else if (SAME_STR(cmd, DOOR_BELL_DISABLE_RTP_TRANSFORMER)) {
    if (rtp_transformer_enabled) {
      rtp_transformer_enabled = false;
      ESP_LOGI(TAG, "RTP transformer disabled via command");
    }
  }
  return 0;
}

int close_data_channel(int id) {
  if (id < ELEMS(user_ch) && user_ch[id].used) {
    ESP_LOGI(TAG, "Start to Close data channel %s", user_ch[id].info.label);
    esp_peer_handle_t peer_handle = NULL;
    esp_webrtc_get_peer_connection(webrtc, &peer_handle);
    esp_peer_close_data_channel(peer_handle, user_ch[id].info.label);
  }
  return 0;
}

static void add_channel(esp_peer_data_channel_info_t* ch) {
  for (int i = 0; i < ELEMS(user_ch); i++) {
    if (user_ch[i].used == false) {
      user_ch[i].used = true;
      char def_label[2] = "0";
      def_label[0] += i;
      user_ch[i].info.label = strdup(ch->label ? ch->label : def_label);
      user_ch[i].info.stream_id = ch->stream_id;
      user_ch[i].send_count = 0;
      user_ch[i].recv_count = 0;
      break;
    }
  }
}

user_data_ch_t* get_channel(uint16_t stream_id) {
  for (int i = 0; i < ELEMS(user_ch); i++) {
    if (user_ch[i].used && user_ch[i].info.stream_id == stream_id) {
      return &user_ch[i];
    }
  }
  return NULL;
}

static void remove_channel(esp_peer_data_channel_info_t* ch) {
  for (int i = 0; i < ELEMS(user_ch); i++) {
    if (user_ch[i].used && user_ch[i].info.stream_id == ch->stream_id) {
      user_ch[i].used = false;
      ESP_LOGI(TAG, "Removed %s id %d finished", user_ch[i].info.label, ch->stream_id);
      free((char*)user_ch[i].info.label);
      user_ch[i].info.label = NULL;
      break;
    }
  }
}

#define CNT_TO_CHAR(c) (((c)&0xFF) % 94 + 33)

static void data_thread_hdlr(void* arg) {
  data_running = true;
#define SEND_PERIOD 1000
  int time = 0;
  int last_send_time = -SEND_PERIOD;
  int str_len = 8192;
  char* str = calloc(1, 8192);
  while (webrtc) {
    bool need_send = false;
    for (int i = 0; i < ELEMS(user_ch); i++) {
      if (user_ch[i].used && time >= last_send_time + SEND_PERIOD) {
        need_send = true;
      }
    }
    if (need_send) {
      for (int i = 0; i < ELEMS(user_ch); i++) {
        if (user_ch[i].used == false) {
          continue;
        }
        int n = snprintf(str, str_len - 1, "Send to %s count %d\n", user_ch[i].info.label, user_ch[i].send_count);
        memset(str + n, CNT_TO_CHAR(user_ch[i].send_count), str_len - n);
        user_ch[i].send_count++;
        esp_peer_data_frame_t data_frame = {
            .type = ESP_PEER_DATA_CHANNEL_STRING,
            .stream_id = user_ch[i].info.stream_id,
            .data = (uint8_t*)str,
            .size = str_len,
        };
        esp_peer_handle_t peer_handle = NULL;
        esp_webrtc_get_peer_connection(webrtc, &peer_handle);
        esp_peer_send_data(peer_handle, &data_frame);
        printf("Send string %.*s", n, str);
      }
      last_send_time = time;
    }
    media_lib_thread_sleep(50);
    time += 50;
    ;
  }
  data_running = false;
  media_lib_thread_destroy(NULL);
}

static int webrtc_data_channel_opened(esp_peer_data_channel_info_t* ch, void* ctx) {
  imu_data_channel_connected = true;
  ESP_LOGI(TAG, "Channel %s opened stream id %d", ch->label ? ch->label : "NULL", ch->stream_id);
  ESP_LOGI(TAG, "IMU sync data channel ready label=%s stream_id=%d", ch->label ? ch->label : "NULL", ch->stream_id);
  add_channel(ch);
  return 0;
}

static bool verify_data(uint8_t* data, int size) {
  char* line_end = strchr((char*)data, '\n');
  char* count = strstr((char*)data, "the ");
  if (line_end == NULL || count == NULL) {
    return false;
  }
  int n = (line_end - (char*)data) + 1;
  int left_size = size - n;
  int send_count = atoi(count + strlen("the "));
  uint8_t expect = CNT_TO_CHAR(send_count);
  for (int i = 0; i < left_size; i++) {
    if (data[n + i] != expect) {
      return false;
    }
  }
  return true;
}

static int webrtc_on_data(esp_peer_data_frame_t* frame, void* ctx) {
  user_data_ch_t* ch = get_channel(frame->stream_id);
  char* line_end = strchr((char*)frame->data, '\n');
  if (line_end == NULL) {
    return -1;
  }
  int str_len = (line_end - (char*)frame->data);
  bool verified = verify_data(frame->data, frame->size);
  if (verified == false) {
    ESP_LOGE(TAG, "Get data label %s verify:%d data: %.*s", ch && ch->info.label ? ch->info.label : "NULL", verified,
             str_len, (char*)frame->data);
  } else {
    ESP_LOGI(TAG, "Get data label %s verify:%d data: %.*s", ch && ch->info.label ? ch->info.label : "NULL", verified,
             str_len, (char*)frame->data);
  }
  return 0;
}

static int webrtc_data_channel_closed(esp_peer_data_channel_info_t* ch, void* ctx) {
  imu_data_channel_connected = false;
  ESP_LOGW(TAG, "IMU sync data channel closed label=%s stream_id=%d", ch->label ? ch->label : "NULL", ch->stream_id);
  remove_channel(ch);
  return 0;
}

static int rtp_sender_get_encoded_size(esp_peer_rtp_frame_t* frame, bool* in_place, void* ctx) {
  rtp_transformer_ctx_t* t_ctx = (rtp_transformer_ctx_t*)ctx;
  // Skip for video payload
  if (frame->payload_type >= 96) {
    return ESP_PEER_ERR_NOT_SUPPORT;
  }
  int tail_size = snprintf(NULL, 0, "TAIL_%" PRIu32, t_ctx->frame_count);
  // Make sure encoded size < one MTU
  frame->encoded_size = frame->orig_size + sizeof(uint32_t) * 2 + tail_size + 1;
  if (frame->encoded_size > 1300) {
    return ESP_PEER_ERR_NOT_SUPPORT;
  }
  // Not support in place transform
  *in_place = false;
  return 0;
}

static int rtp_sender_transform(esp_peer_rtp_frame_t* frame, void* ctx) {
  rtp_transformer_ctx_t* t_ctx = (rtp_transformer_ctx_t*)ctx;
  // Skip for video payload
  if (frame->payload_type >= 96 || frame->orig_size < RTP_HEADER_SIZE) {
    return ESP_PEER_ERR_NOT_SUPPORT;
  }
  char tail_string[64];
  int written = snprintf(tail_string, sizeof(tail_string), "TAIL_%" PRIu32, t_ctx->frame_count);
  tail_string[written] = '\0';
  uint32_t tail_len = written + 1;
  // Build new payload: [4-byte size][original payload][tail string]
  uint32_t new_size = frame->orig_size + sizeof(uint32_t) * 2 + tail_len;
  uint8_t* new_data = frame->encoded_data;
  int src_ofst = 0;
  int dst_ofst = 0;
  memcpy(new_data + dst_ofst, frame->orig_data + src_ofst, RTP_HEADER_SIZE);
  dst_ofst += RTP_HEADER_SIZE;
  src_ofst += RTP_HEADER_SIZE;

  uint32_t payload_size = frame->orig_size - RTP_HEADER_SIZE;
  memcpy(new_data + dst_ofst, &payload_size, sizeof(uint32_t));
  dst_ofst += sizeof(uint32_t);

  memcpy(new_data + dst_ofst, frame->orig_data + src_ofst, payload_size);
  dst_ofst += payload_size;
  memcpy(new_data + dst_ofst, &tail_len, sizeof(uint32_t));
  dst_ofst += sizeof(uint32_t);
  memcpy(new_data + dst_ofst, tail_string, tail_len);
  dst_ofst += tail_len;
  frame->encoded_size = new_size;
  t_ctx->frame_count++;
  if (t_ctx->frame_count % 100 == 0) {
    ESP_LOGI(TAG, "[SENDER] RTP transformed: frame_count=%u, error_count=%u", t_ctx->frame_count, t_ctx->error_count);
  }
  return 0;
}

static int rtp_receiver_get_encoded_size(esp_peer_rtp_frame_t* frame, bool* in_place, void* ctx) {
  rtp_transformer_ctx_t* t_ctx = (rtp_transformer_ctx_t*)ctx;
  // Skip for video payload
  if (frame->payload_type >= 96) {
    return ESP_PEER_ERR_NOT_SUPPORT;
  }
  uint32_t original_size = 0;
  memcpy(&original_size, frame->orig_data + RTP_HEADER_SIZE, sizeof(uint32_t));
  if (RTP_HEADER_SIZE + original_size + sizeof(uint32_t) * 2 > frame->orig_size) {
    t_ctx->error_count++;
    return ESP_PEER_ERR_BAD_DATA;
  }
  uint32_t tail_size = 0;
  memcpy(&tail_size, frame->orig_data + RTP_HEADER_SIZE + original_size + sizeof(uint32_t), sizeof(uint32_t));
  uint32_t encoded_size = RTP_HEADER_SIZE + original_size + sizeof(uint32_t) * 2 + tail_size;
  if (encoded_size != frame->orig_size) {
    t_ctx->error_count++;
    return ESP_PEER_ERR_BAD_DATA;
  }
  frame->encoded_size = RTP_HEADER_SIZE + original_size;
  *in_place = true;
  return 0;
}

static int rtp_receiver_transform(esp_peer_rtp_frame_t* frame, void* ctx) {
  rtp_transformer_ctx_t* t_ctx = (rtp_transformer_ctx_t*)ctx;
  uint32_t original_size = 0;
  uint8_t* original_data = NULL;
  uint32_t tail_size = 0;
  int ofst = RTP_HEADER_SIZE;
  memcpy(&original_size, frame->orig_data + ofst, sizeof(uint32_t));
  ofst += sizeof(uint32_t);
  original_data = frame->orig_data + ofst;
  ofst += original_size;
  memcpy(&tail_size, frame->orig_data + ofst, sizeof(uint32_t));
  ofst += sizeof(uint32_t);
  char* tail_str = (char*)frame->orig_data + ofst;
  t_ctx->frame_count++;
  if (strncmp(tail_str, "TAIL_", 5) != 0) {
    t_ctx->error_count++;
    return ESP_PEER_ERR_BAD_DATA;
  }
  if (frame->encoded_data != frame->orig_data) {
    memcpy(frame->encoded_data, frame->orig_data, RTP_HEADER_SIZE);
    memcpy(frame->encoded_data + RTP_HEADER_SIZE, original_data, original_size);
  } else {
    memmove(frame->orig_data + RTP_HEADER_SIZE, original_data, original_size);
  }
  frame->encoded_size = RTP_HEADER_SIZE + original_size;
  if (t_ctx->frame_count % 100 == 0) {
    ESP_LOGI(TAG, "[RECEIVER] RTP transformed: frame_count=%u, error_count=%u", t_ctx->frame_count, t_ctx->error_count);
  }
  return 0;
}

static void setup_rtp_transformers(esp_peer_handle_t peer_handle) {
  // Set up sender transformer
  esp_peer_rtp_transform_cb_t sender_transform_cb = {
      .get_encoded_size = rtp_sender_get_encoded_size,
      .transform = rtp_sender_transform,
  };
  int ret = esp_peer_set_rtp_transformer(peer_handle, ESP_PEER_RTP_TRANSFORM_ROLE_SENDER,
                                         rtp_transformer_enabled ? &sender_transform_cb : NULL,
                                         rtp_transformer_enabled ? &sender_transformer_ctx : NULL);
  if (ret == ESP_PEER_ERR_NONE) {
    ESP_LOGI(TAG, "RTP sender transformer set up successfully");
  } else {
    ESP_LOGE(TAG, "Failed to set up RTP sender transformer: %d", ret);
  }
  // Set up receiver transformer
  esp_peer_rtp_transform_cb_t receiver_transform_cb = {
      .get_encoded_size = rtp_receiver_get_encoded_size,
      .transform = rtp_receiver_transform,
  };
  ret = esp_peer_set_rtp_transformer(peer_handle, ESP_PEER_RTP_TRANSFORM_ROLE_RECEIVER,
                                     rtp_transformer_enabled ? &receiver_transform_cb : NULL,
                                     rtp_transformer_enabled ? &receiver_transformer_ctx : NULL);
  if (ret == ESP_PEER_ERR_NONE) {
    ESP_LOGI(TAG, "RTP receiver transformer set up successfully");
  } else {
    ESP_LOGE(TAG, "Failed to set up RTP receiver transformer: %d", ret);
  }
}

static int webrtc_event_handler(esp_webrtc_event_t* event, void* ctx) {
  if (event->type == ESP_WEBRTC_EVENT_CONNECTED) {
    door_bell_change_state(DOOR_BELL_STATE_CONNECTED);
    webrtc_connected = true;
    request_imu_data_channel("peer_connected");
    if (webrtc) {
      esp_webrtc_set_video_bitrate(webrtc, WEBRTC_VIDEO_BITRATE_STABLE);
      esp_webrtc_set_audio_bitrate(webrtc, WEBRTC_AUDIO_BITRATE);
      ESP_LOGI(TAG, "Applied stable bitrate profile: video=%d audio=%d", WEBRTC_VIDEO_BITRATE_STABLE,
               WEBRTC_AUDIO_BITRATE);
    }
  } else if (event->type == ESP_WEBRTC_EVENT_CONNECT_FAILED || event->type == ESP_WEBRTC_EVENT_DISCONNECTED) {
    door_bell_change_state(DOOR_BELL_STATE_NONE);
    webrtc_connected = false;
    reset_imu_sync_state();
    // Reset transformer contexts
    memset(&sender_transformer_ctx, 0, sizeof(sender_transformer_ctx));
    memset(&receiver_transformer_ctx, 0, sizeof(receiver_transformer_ctx));
#if CONFIG_DOORBELL_SIGNALING_JANUS
    cleanup_webrtc_handle();
    schedule_janus_retry();
#endif
  } else if (event->type == ESP_WEBRTC_EVENT_PAIRED) {
    esp_peer_handle_t peer_handle = NULL;
    esp_webrtc_get_peer_connection(webrtc, &peer_handle);
    setup_rtp_transformers(peer_handle);
    esp_peer_addr_t addr = {};
    esp_peer_get_paired_addr(peer_handle, &addr);
    ESP_LOGI(TAG, "Paired with %d.%d.%d.%d:%d", addr.ipv4[0], addr.ipv4[1], addr.ipv4[2], addr.ipv4[3], addr.port);
  } else if (event->type == ESP_WEBRTC_EVENT_DATA_CHANNEL_CONNECTED ||
             event->type == ESP_WEBRTC_EVENT_DATA_CHANNEL_OPENED) {
    imu_data_channel_connected = true;
    ESP_LOGI(TAG, "IMU sync data channel state event=%d body=%s", event->type, event->body ? event->body : "NULL");
  } else if (event->type == ESP_WEBRTC_EVENT_DATA_CHANNEL_DISCONNECTED ||
             event->type == ESP_WEBRTC_EVENT_DATA_CHANNEL_CLOSED) {
    imu_data_channel_connected = false;
    ESP_LOGW(TAG, "IMU sync data channel state event=%d body=%s", event->type, event->body ? event->body : "NULL");
  }
  return 0;
}

void send_cmd(char* cmd) {
  if (!cmd) {
    return;
  }
  if (ProcessServoConfigCommand(cmd)) {
    return;
  }
  if (ProcessServoCommand(cmd)) {
    ESP_LOGI(TAG, "Servo command via control: %s", cmd);
    return;
  }
  if (ProcessMoveCommand(cmd)) {
    ESP_LOGI(TAG, "Move command via control: %s", cmd);
    return;
  }
  if (SAME_STR(cmd, "ring")) {
    SEND_CMD(webrtc, DOOR_BELL_RING_CMD);
    ESP_LOGI(TAG, "Ring button on state %d", door_bell_state);
    if (door_bell_state < DOOR_BELL_STATE_CONNECTING) {
      door_bell_state = DOOR_BELL_STATE_RINGING;
      play_tone(DOOR_BELL_TONE_RING);
    }
  }
}

static void key_monitor_thread(void* arg) {
  gpio_config_t io_conf;
  memset(&io_conf, 0, sizeof(io_conf));
  io_conf.mode = GPIO_MODE_INPUT;
  io_conf.pin_bit_mask = BIT64(DOOR_BELL_RING_BUTTON);
  io_conf.pull_down_en = 1;
  esp_err_t ret = 0;
  ret |= gpio_config(&io_conf);

  media_lib_thread_sleep(50);
  int last_level = gpio_get_level(DOOR_BELL_RING_BUTTON);
  int init_level = last_level;

  while (monitor_key) {
    media_lib_thread_sleep(50);
    int level = gpio_get_level(DOOR_BELL_RING_BUTTON);
    if (level != last_level) {
      last_level = level;
      if (level != init_level) {
        send_cmd("ring");
      }
    }
  }
  media_lib_thread_destroy(NULL);
}

int start_webrtc(char* url) {
  if (network_is_connected() == false) {
    ESP_LOGE(TAG, "Wifi not connected yet");
    return -1;
  }
#if CONFIG_DOORBELL_SIGNALING_JANUS
  janus_retry_stop = false;
#endif
  if (webrtc) {
    webrtc_connected = false;
    cleanup_webrtc_handle();
  }
  reset_imu_sync_state();
  monitor_key = true;
  media_lib_thread_handle_t key_thread;
  media_lib_thread_create_from_scheduler(&key_thread, "Key", key_monitor_thread, NULL);

  const char* signal_url = url;
#if CONFIG_DOORBELL_SIGNALING_JANUS
  if (signal_url == NULL || signal_url[0] == '\0') {
    signal_url = WEBRTC_SIGNAL_URL;
  }
  esp_peer_signaling_janus_cfg_t janus_cfg = {
      .room_id = WEBRTC_JANUS_ROOM_ID,
      .token = strlen(WEBRTC_JANUS_TOKEN) ? WEBRTC_JANUS_TOKEN : NULL,
      .pin = strlen(WEBRTC_JANUS_ROOM_PIN) ? WEBRTC_JANUS_ROOM_PIN : NULL,
      .display = strlen(WEBRTC_JANUS_DISPLAY) ? WEBRTC_JANUS_DISPLAY : NULL,
      .api_secret = strlen(WEBRTC_JANUS_API_SECRET) ? WEBRTC_JANUS_API_SECRET : NULL,
  };
#endif

  esp_peer_default_cfg_t peer_cfg = {
      .agent_recv_timeout = 1200,
  };
  esp_webrtc_cfg_t cfg = {
      .peer_cfg =
          {
              .audio_info =
                  {
#ifdef WEBRTC_SUPPORT_OPUS
                      .codec = ESP_PEER_AUDIO_CODEC_OPUS,
                      .sample_rate = 16000,
                      .channel = 2,
#else
                      .codec = ESP_PEER_AUDIO_CODEC_G711A,
                      .sample_rate = 8000,
                      .channel = 1,
#endif
                  },
              .video_info =
                  {
                      .codec = ESP_PEER_VIDEO_CODEC_H264,
                      .width = VIDEO_WIDTH,
                      .height = VIDEO_HEIGHT,
                      .fps = VIDEO_FPS,
                  },
              .audio_dir = ESP_PEER_MEDIA_DIR_SEND_ONLY,
              .video_dir = ESP_PEER_MEDIA_DIR_SEND_ONLY,
              .on_custom_data = door_bell_on_cmd,
              // Add following data channel callback for more accurate control over data channel
              .on_channel_open = webrtc_data_channel_opened,
              .on_data = webrtc_on_data,
              .on_channel_close = webrtc_data_channel_closed,
              .on_video_send = send_imu_synced_with_video,
              .enable_data_channel = true,
              .manual_ch_create = false,  // Auto-create data channel so IMU stream has an active SCTP channel
              .no_auto_reconnect =
#if CONFIG_DOORBELL_SIGNALING_JANUS
                  false,  // Janus test mode: auto connect without ACCEPT_CALL gating
#else
                  true,  // No auto connect peer when signaling connected
#endif
              .extra_cfg = &peer_cfg,
              .extra_size = sizeof(peer_cfg),
          },
      .signaling_cfg =
          {
              .signal_url = (char*)signal_url,
#if CONFIG_DOORBELL_SIGNALING_JANUS
              .extra_cfg = &janus_cfg,
              .extra_size = sizeof(janus_cfg),
#endif
          },
      .peer_impl = esp_peer_get_default_impl(),
#if CONFIG_DOORBELL_SIGNALING_JANUS
      .signaling_impl = esp_signaling_get_janus_impl(),
#else
      .signaling_impl = esp_signaling_get_http_impl(),
#endif
  };
  int ret = esp_webrtc_open(&cfg, &webrtc);
  if (ret != 0) {
    ESP_LOGE(TAG, "Fail to open webrtc");
#if CONFIG_DOORBELL_SIGNALING_JANUS
    cleanup_webrtc_handle();
    schedule_janus_retry();
#endif
    return ret;
  }
  // Set media provider
  esp_webrtc_media_provider_t media_provider = {};
  media_sys_get_provider(&media_provider);
  esp_webrtc_set_media_provider(webrtc, &media_provider);

  // if (network_is_good()) {
  //   esp_webrtc_set_video_bitrate(rtc_handle, 1000000); // 1 Mbps
  // } else {
  //   esp_webrtc_set_video_bitrate(rtc_handle, 300000);  // 300 kbps
  // }
  esp_webrtc_set_video_bitrate(webrtc, WEBRTC_VIDEO_BITRATE_START);
  esp_webrtc_set_audio_bitrate(webrtc, WEBRTC_AUDIO_BITRATE);

  // Set event handler
  esp_webrtc_set_event_handler(webrtc, webrtc_event_handler, NULL);

  // For Janus publishing tests, auto-enable peer connection immediately.
#if CONFIG_DOORBELL_SIGNALING_JANUS
  esp_webrtc_enable_peer_connection(webrtc, true);
#else
  // Default disable auto connect of peer connection
  esp_webrtc_enable_peer_connection(webrtc, false);
#endif

#if DATA_CHANNEL_STRESS_TEST
  media_lib_thread_handle_t data_thread;
  media_lib_thread_create_from_scheduler(&data_thread, "data", data_thread_hdlr, NULL);
#endif

  // Start webrtc
  ret = esp_webrtc_start(webrtc);
  if (ret != 0) {
    ESP_LOGE(TAG, "Fail to start webrtc");
    cleanup_webrtc_handle();
#if CONFIG_DOORBELL_SIGNALING_JANUS
    schedule_janus_retry();
#endif
  } else {
#if CONFIG_DOORBELL_SIGNALING_JANUS
    // Ensure pending_connect is cleared after signaling startup.
    esp_webrtc_enable_peer_connection(webrtc, true);
#endif
    play_tone(DOOR_BELL_TONE_JOIN_SUCCESS);
  }
  return ret;
}

void query_webrtc(void) {
  if (webrtc) {
    esp_webrtc_query(webrtc);
  }
}

int stop_webrtc(void) {
#if CONFIG_DOORBELL_SIGNALING_JANUS
  janus_retry_stop = true;
#endif
  webrtc_connected = false;
  cleanup_webrtc_handle();
  return 0;
}

bool webrtc_is_running(void) { return webrtc != NULL; }
bool webrtc_is_connected(void) { return webrtc_connected; }
