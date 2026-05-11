#include "mcp2515_canbus.h"

#include <string.h>
#include "config.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mcp2515.h"

#define TAG "MCP2515_CANBUS"
#define CANBUS_MESSAGE_ID 0x1F
#define MCP2515_INIT_RETRY_COUNT 5
#ifndef MCP2515_SPI_CLOCK_HZ
#define MCP2515_SPI_CLOCK_HZ 1000000
#endif

static Mcp2515CanbusMessageHandler s_message_handler = NULL;
static spi_host_device_t s_spi_host = SPI2_HOST;
static bool s_spi_device_added = false;

static void mcp2515_hard_reset_if_available(void) {
  if (MCP2515_RESET_GPIO < 0) {
    return;
  }

  gpio_config_t io_conf = {
      .pin_bit_mask = (1ULL << MCP2515_RESET_GPIO),
      .mode = GPIO_MODE_OUTPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  if (gpio_config(&io_conf) != ESP_OK) {
    ESP_LOGW(TAG, "Failed to config MCP2515_RESET_GPIO=%d", MCP2515_RESET_GPIO);
    return;
  }

  gpio_set_level(MCP2515_RESET_GPIO, 0);
  vTaskDelay(pdMS_TO_TICKS(2));
  gpio_set_level(MCP2515_RESET_GPIO, 1);
  vTaskDelay(pdMS_TO_TICKS(10));
  ESP_LOGW(TAG, "Applied MCP2515 hardware reset on GPIO %d", MCP2515_RESET_GPIO);
}

static CAN_SPEED_t speed_from_config(void) {
  switch (MCP2515_CAN_SPEED_KBPS) {
    case 50:
      return CAN_50KBPS;
    case 100:
      return CAN_100KBPS;
    case 125:
      return CAN_125KBPS;
    case 250:
      return CAN_250KBPS;
    case 500:
    default:
      return CAN_500KBPS;
  }
}

static CAN_CLOCK_t clock_from_config(void) {
  switch (MCP2515_XTAL_MHZ) {
    case 8:
      return MCP_8MHZ;
    case 20:
      return MCP_20MHZ;
    case 16:
    default:
      return MCP_16MHZ;
  }
}

static void config_accept_command_id_only(void) {
#if MCP2515_DEBUG_ACCEPT_ALL
  // Debug mode: accept all incoming frames (standard + extended).
  (void)MCP2515_setFilterMask(MASK0, true, 0x00000000);
  (void)MCP2515_setFilterMask(MASK1, true, 0x00000000);
  (void)MCP2515_setFilter(RXF0, true, 0x00000000);
  (void)MCP2515_setFilter(RXF1, true, 0x00000000);
  (void)MCP2515_setFilter(RXF2, true, 0x00000000);
  (void)MCP2515_setFilter(RXF3, true, 0x00000000);
  (void)MCP2515_setFilter(RXF4, true, 0x00000000);
  (void)MCP2515_setFilter(RXF5, true, 0x00000000);
  ESP_LOGW(TAG, "DEBUG filter mode enabled: accepting all CAN IDs");
#else
  const uint32_t std_mask = CAN_SFF_MASK;
  const uint32_t std_id = CANBUS_MESSAGE_ID;
  (void)MCP2515_setFilterMask(MASK0, false, std_mask);
  (void)MCP2515_setFilterMask(MASK1, false, std_mask);
  (void)MCP2515_setFilter(RXF0, false, std_id);
  (void)MCP2515_setFilter(RXF1, false, std_id);
  (void)MCP2515_setFilter(RXF2, false, std_id);
  (void)MCP2515_setFilter(RXF3, false, std_id);
  (void)MCP2515_setFilter(RXF4, false, std_id);
  (void)MCP2515_setFilter(RXF5, false, std_id);
#endif
}

static void log_can_ctrl_stat(const char* stage) {
  const uint8_t canstat = MCP2515_readRegister(MCP_CANSTAT);
  const uint8_t canctrl = MCP2515_readRegister(MCP_CANCTRL);
  ESP_LOGW(TAG, "%s: CANSTAT=0x%02X CANCTRL=0x%02X", stage, canstat, canctrl);
}

static bool mcp2515_detect_after_reset(void) {
  const uint8_t canstat = MCP2515_readRegister(MCP_CANSTAT);
  const uint8_t canctrl = MCP2515_readRegister(MCP_CANCTRL);
  const bool stat_ok = (canstat & 0xEE) == 0x80;
  const bool ctrl_ok = (canctrl & 0x17) == 0x07;
  if (stat_ok && ctrl_ok) {
    return true;
  }

  ESP_LOGW(TAG, "Detect defaults mismatch: CANSTAT=0x%02X CANCTRL=0x%02X", canstat, canctrl);

  // Some boards do not present exact reset-defaults reliably; force config mode and verify OPMOD.
  if (MCP2515_setConfigMode() == ERROR_OK) {
    const uint8_t canstat_cfg = MCP2515_readRegister(MCP_CANSTAT);
    if ((canstat_cfg & 0xE0) == CANCTRL_REQOP_CONFIG) {
      ESP_LOGW(TAG, "Detect accepted via setConfigMode: CANSTAT=0x%02X", canstat_cfg);
      return true;
    }
  }

  return false;
}

static void log_can_frame_rx(const CAN_FRAME frame) {
  char payload[3 * CAN_MAX_DLEN + 1] = {0};
  int pos = 0;
  for (int i = 0; i < frame->can_dlc && i < CAN_MAX_DLEN; i++) {
    int n = snprintf(payload + pos, sizeof(payload) - pos, "%02X%s", frame->data[i],
                     (i == frame->can_dlc - 1) ? "" : " ");
    if (n <= 0 || (size_t)n >= (sizeof(payload) - pos)) {
      break;
    }
    pos += n;
  }

  const bool ext = (frame->can_id & CAN_EFF_FLAG) != 0;
  const uint32_t id = frame->can_id & (ext ? CAN_EFF_MASK : CAN_SFF_MASK);
  ESP_LOGI(TAG, "RX CAN ext=%d ID=0x%lX DLC=%d DATA=[%s]", ext, (unsigned long)id, frame->can_dlc, payload);
}

static void can_receive_task(void* param) {
  CAN_FRAME_t frame_buf;
  CAN_FRAME frame = frame_buf;
  memset(frame, 0, sizeof(frame_buf));
  TickType_t last_err_check = 0;
  TickType_t last_err_log = 0;
  TickType_t last_rx_diag_log = 0;
  uint32_t rx_count = 0;
  uint32_t matched_count = 0;

  while (1) {
    if (MCP2515_checkReceive()) {
      ERROR_t err = MCP2515_readMessageAfterStatCheck(frame);
      if (err == ERROR_OK) {
        rx_count++;
        log_can_frame_rx(frame);
        const bool ext = (frame->can_id & CAN_EFF_FLAG) != 0;
        const uint32_t id = frame->can_id & (ext ? CAN_EFF_MASK : CAN_SFF_MASK);
        if (!ext && id == CANBUS_MESSAGE_ID && frame->can_dlc > 0 && s_message_handler) {
          matched_count++;
          s_message_handler(frame->can_dlc, frame->data);
        } else if (s_message_handler) {
          ESP_LOGW(TAG, "Ignoring frame: ext=%d id=0x%lX (expect std 0x%03X)", ext, (unsigned long)id,
                   CANBUS_MESSAGE_ID);
        }
      } else if (err != ERROR_NOMSG) {
        ESP_LOGW(TAG, "Read CAN frame failed: %d", err);
      }
    }

    TickType_t now = xTaskGetTickCount();
    if ((now - last_err_check) >= pdMS_TO_TICKS(100)) {
      last_err_check = now;
      if (MCP2515_checkError()) {
        uint8_t eflg = MCP2515_getErrorFlags();
        if ((now - last_err_log) >= pdMS_TO_TICKS(1000)) {
          ESP_LOGW(TAG, "MCP2515 error flag=0x%02X", eflg);
          last_err_log = now;
        }
        MCP2515_clearRXnOVR();
        MCP2515_clearMERR();
        MCP2515_clearERRIF();
      }
    }

    if ((now - last_rx_diag_log) >= pdMS_TO_TICKS(1000)) {
      last_rx_diag_log = now;
      ESP_LOGI(TAG, "RX diag: total=%lu matched=%lu CANINTF=0x%02X EFLG=0x%02X",
               (unsigned long)rx_count, (unsigned long)matched_count, MCP2515_getInterrupts(),
               MCP2515_getErrorFlags());
    }

    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

void Mcp2515CanbusSetMessageHandler(Mcp2515CanbusMessageHandler handler) {
  s_message_handler = handler;
}

void Mcp2515CanbusInit(void) {
  esp_err_t ret = ESP_OK;
  int selected_spi_mode = -1;

  const spi_bus_config_t buscfg = {
      .mosi_io_num = PIN_NUM_MOSI,
      .miso_io_num = PIN_NUM_MISO,
      .sclk_io_num = PIN_NUM_CLK,
      .quadwp_io_num = -1,
      .quadhd_io_num = -1,
      .max_transfer_sz = 64,
  };

  spi_device_interface_config_t devcfg = {
      .clock_speed_hz = MCP2515_SPI_CLOCK_HZ,
      .mode = 0,
      .cs_ena_pretrans = 2,
      .cs_ena_posttrans = 2,
      .spics_io_num = PIN_NUM_CS,
      .queue_size = 4,
  };

  ret = spi_bus_initialize(s_spi_host, &buscfg, SPI_DMA_CH_AUTO);
  if (ret == ESP_ERR_INVALID_STATE) {
    ESP_LOGW(TAG, "SPI bus already initialized, continue");
  } else {
    ESP_ERROR_CHECK(ret);
  }

  if (MCP2515_Object == NULL) {
    if (MCP2515_init() != ERROR_OK) {
      ESP_LOGE(TAG, "MCP2515_init failed");
      return;
    }
  }

  mcp2515_hard_reset_if_available();

  bool reset_ok = false;
  const int spi_modes[] = {0, 3};
  ESP_LOGW(TAG, "MCP2515 SPI pins: MISO=%d MOSI=%d SCLK=%d CS=%d clk=%dHz", PIN_NUM_MISO, PIN_NUM_MOSI,
           PIN_NUM_CLK, PIN_NUM_CS, MCP2515_SPI_CLOCK_HZ);
  for (size_t m = 0; m < (sizeof(spi_modes) / sizeof(spi_modes[0])) && !reset_ok; m++) {
    if (s_spi_device_added) {
      spi_bus_remove_device(MCP2515_Object->spi);
      MCP2515_Object->spi = NULL;
      s_spi_device_added = false;
    }

    devcfg.mode = spi_modes[m];
    ret = spi_bus_add_device(s_spi_host, &devcfg, &MCP2515_Object->spi);
    if (ret != ESP_OK) {
      ESP_LOGE(TAG, "spi_bus_add_device(mode=%d) failed: %s", devcfg.mode, esp_err_to_name(ret));
      return;
    }
    s_spi_device_added = true;

    ESP_LOGW(TAG, "Trying MCP2515 with SPI mode %d", devcfg.mode);
    for (int i = 0; i < MCP2515_INIT_RETRY_COUNT; i++) {
      const ERROR_t rc = MCP2515_reset();
      if (rc == ERROR_OK && mcp2515_detect_after_reset()) {
        reset_ok = true;
        selected_spi_mode = devcfg.mode;
        break;
      }
      log_can_ctrl_stat("reset retry");
      vTaskDelay(pdMS_TO_TICKS(20));
    }
  }

  if (!reset_ok) {
    ESP_LOGE(TAG, "MCP2515 reset/detect failed in SPI modes 0 and 3. Check MISO/MOSI/SCK/CS/RESET wiring.");
    return;
  }

  if (MCP2515_setBitrate(speed_from_config(), clock_from_config()) != ERROR_OK) {
    ESP_LOGE(TAG, "MCP2515_setBitrate failed (CAN=%d kbps XTAL=%d MHz)",
             MCP2515_CAN_SPEED_KBPS, MCP2515_XTAL_MHZ);
    log_can_ctrl_stat("setBitrate fail");
    return;
  }

  config_accept_command_id_only();

  if (MCP2515_setNormalMode() != ERROR_OK) {
    ESP_LOGE(TAG, "MCP2515_setNormalMode failed");
    log_can_ctrl_stat("normal mode fail");
    return;
  }

  ESP_LOGI(TAG, "MCP2515 ready: CAN %d kbps, XTAL %d MHz, ID filter=0x%03X",
           MCP2515_CAN_SPEED_KBPS, MCP2515_XTAL_MHZ, CANBUS_MESSAGE_ID);
  ESP_LOGI(TAG, "MCP2515 SPI mode selected: %d", selected_spi_mode);

  xTaskCreatePinnedToCore(can_receive_task, "mcp2515_can_rx", 4096, NULL, 2, NULL, 1);
}
