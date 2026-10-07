#include "espnow_actuator.h"
#include "actuator_config.h"
#include "serial_status.h"

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <freertos/semphr.h>

static bool s_ready = false;
static bool s_isTx = false;
static EspnowActuatorRecvFn s_recvFn = nullptr;
static uint32_t s_txSeq = 0;
static uint32_t s_txOk = 0;
static uint32_t s_txFail = 0;
static uint32_t s_rxBad = 0;
static uint32_t s_lastRxSeq = 0;
static uint32_t s_lastOrderedRxMs = 0;
static bool s_haveRxSeq = false;
static SemaphoreHandle_t s_sendMutex = nullptr;
static portMUX_TYPE s_seqMux = portMUX_INITIALIZER_UNLOCKED;

static uint32_t nextTxSeq() {
  portENTER_CRITICAL(&s_seqMux);
  const uint32_t seq = ++s_txSeq;
  portEXIT_CRITICAL(&s_seqMux);
  return seq;
}

static const uint8_t kBroadcastMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

static void formatMac(char *out, size_t outLen, const uint8_t mac[6]) {
  snprintf(out, outLen, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void onEspnowSend(const uint8_t *mac, esp_now_send_status_t status) {
  // This fires for EVERY packet. With all keys held the controller sends RGB sync at
  // 20Hz on top of colour packets, and formatting a MAC + printf per send -- in the
  // WiFi task -- was enough to stutter MP3 playback. Failures still log every time;
  // successes are counted and summarised occasionally.
  if (status == ESP_NOW_SEND_SUCCESS) {
    ++s_txOk;
    if ((s_txOk % 200u) == 0u)
      Serial.printf("[ESPNOW] send_cb ok=%u fail=%u\n", (unsigned)s_txOk, (unsigned)s_txFail);
    return;
  }
  ++s_txFail;
  char macStr[18];
  formatMac(macStr, sizeof(macStr), mac);
  Serial.printf("[ESPNOW] send_cb FAIL mac=%s  ok=%u fail=%u\n", macStr, (unsigned)s_txOk, (unsigned)s_txFail);
}

static void applyWifiChannel(uint8_t channel) {
  if (channel < 1 || channel > 13)
    return;
  esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
}

void espnowActuatorSetWifiChannel(uint8_t channel) {
  applyWifiChannel(channel);
  Serial.printf("[ESPNOW] channel=%u\n", (unsigned)channel);
}

static bool addBroadcastPeer() {
  if (esp_now_is_peer_exist(kBroadcastMac))
    return true;

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, kBroadcastMac, 6);
  peer.channel = ESPNOW_WIFI_CHANNEL;
  peer.encrypt = false;
  const esp_err_t err = esp_now_add_peer(&peer);
  if (err != ESP_OK) {
    Serial.printf("[ESPNOW] add_peer broadcast failed err=%d\n", (int)err);
    return false;
  }
  return true;
}

static bool espnowCoreInit(bool tx) {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(false, true);
  applyWifiChannel(ESPNOW_WIFI_CHANNEL);

  if (esp_now_init() != ESP_OK) {
    Serial.println("[ESPNOW] init failed");
    return false;
  }

  s_isTx = tx;
  if (tx && !s_sendMutex) {
    s_sendMutex = xSemaphoreCreateMutex();
    if (!s_sendMutex) {
      Serial.println("[ESPNOW] send mutex allocation failed");
      esp_now_deinit();
      s_ready = false;
      return false;
    }
  }
  s_ready = true;
  Serial.printf("[ESPNOW] %s ready ch=%u mac=%s\n", tx ? "TX" : "RX", (unsigned)ESPNOW_WIFI_CHANNEL,
                WiFi.macAddress().c_str());
  return true;
}

static void onEspnowRecv(const uint8_t *mac, const uint8_t *data, int len) {
  if (!s_recvFn) {
    ++s_rxBad;
    Serial.printf("[ESPNOW] rx ignored (no handler) len=%d bad=%u\n", len, (unsigned)s_rxBad);
    return;
  }
  if (len != (int)sizeof(ActuatorCmdPacket)) {
    ++s_rxBad;
    Serial.printf("[ESPNOW] rx bad_len=%d (want %u) bad=%u\n", len, (unsigned)sizeof(ActuatorCmdPacket),
                  (unsigned)s_rxBad);
    return;
  }
  ActuatorCmdPacket pkt;
  memcpy(&pkt, data, sizeof(pkt));
  if (!actuatorPacketValid(&pkt)) {
    ++s_rxBad;
    Serial.printf("[ESPNOW] rx invalid pkt magic=0x%02X ver=%u color=%u on=%u bad=%u\n", (unsigned)pkt.magic,
                  (unsigned)pkt.version, (unsigned)pkt.color, (unsigned)pkt.on, (unsigned)s_rxBad);
    return;
  }
  // Broadcast delivery can be delayed/reordered. Without this guard an old ON
  // arriving after its OFF briefly re-energizes a pump (most often noticed on
  // red). Permit a fresh sequence after radio silence so a controller reboot,
  // whose counter restarts at zero, is accepted promptly.
  const uint32_t now = millis();
  if (s_haveRxSeq && now - s_lastOrderedRxMs < (uint32_t)ACTUATOR_RX_FAILSAFE_MS &&
      (int32_t)(pkt.seq - s_lastRxSeq) <= 0) {
    ++s_rxBad;
    Serial.printf("[ESPNOW] rx stale seq=%u last=%u bad=%u\n", (unsigned)pkt.seq, (unsigned)s_lastRxSeq,
                  (unsigned)s_rxBad);
    return;
  }
  s_lastRxSeq = pkt.seq;
  s_lastOrderedRxMs = now;
  s_haveRxSeq = true;
  char macStr[18];
  formatMac(macStr, sizeof(macStr), mac);
  if (pkt.on == ACTUATOR_ON_BUBBLE_PARTY) {
    Serial.printf("[ESPNOW] rx BUBBLE_PARTY R=%u G=%u B=%u W=%u amber=%u seq=%u\n", (unsigned)pkt.level_r,
                  (unsigned)pkt.level_g, (unsigned)pkt.level_b, (unsigned)pkt.level_w, (unsigned)pkt.color,
                  (unsigned)pkt.seq);
  } else if (pkt.on == ACTUATOR_ON_BUBBLE_KILL) {
    Serial.printf("[ESPNOW] rx BUBBLE_KILL seq=%u\n", (unsigned)pkt.seq);
  } else if (pkt.on == ACTUATOR_ON_IDLE_DMX) {
    Serial.printf("[ESPNOW] rx IDLE_DMX R=%u G=%u B=%u seq=%u\n", (unsigned)pkt.level_r, (unsigned)pkt.level_g,
                  (unsigned)pkt.level_b, (unsigned)pkt.seq);
  } else if (pkt.on == ACTUATOR_ON_IDLE_END) {
    Serial.printf("[ESPNOW] rx IDLE_END seq=%u\n", (unsigned)pkt.seq);
  } else if (pkt.on == ACTUATOR_ON_RGB_HOLD) {
    // 20 Hz during a hold — log 1 in 20 so it does not bury [ACT]/[RLY]/[SAFE].
    static uint8_t rgbHoldLogSkip = 0;
    if (++rgbHoldLogSkip >= 20) {
      rgbHoldLogSkip = 0;
      Serial.printf("[ESPNOW] rx RGB_HOLD R=%u G=%u B=%u amber=%u seq=%u (1/20)\n", (unsigned)pkt.level_r,
                    (unsigned)pkt.level_g, (unsigned)pkt.level_b, (unsigned)pkt.color, (unsigned)pkt.seq);
    }
  } else if (pkt.on == ACTUATOR_ON_PURGE) {
    Serial.printf("[ESPNOW] rx PURGE seq=%u\n", (unsigned)pkt.seq);
  } else if (pkt.on == ACTUATOR_ON_PRIME) {
    Serial.printf("[ESPNOW] rx PRIME %s seq=%u\n", pkt.level_g ? "ON" : "OFF", (unsigned)pkt.seq);
  } else {
    Serial.printf("[ESPNOW] rx from %s color=%s on=%u seq=%u\n", macStr, serialStatusColorNameU8(pkt.color),
                  (unsigned)pkt.on, (unsigned)pkt.seq);
  }
  s_recvFn(&pkt, mac);
}

bool espnowActuatorBeginTx() {
  if (!espnowCoreInit(true))
    return false;
  esp_now_register_send_cb(onEspnowSend);
  return addBroadcastPeer();
}

bool espnowActuatorBeginRx(EspnowActuatorRecvFn onCmd) {
  s_recvFn = onCmd;
  if (!espnowCoreInit(false))
    return false;
  esp_now_register_recv_cb(onEspnowRecv);
  return true;
}

bool espnowActuatorReady() { return s_ready; }

bool espnowActuatorSend(ActuatorCmdPacket *pkt) {
  if (!s_ready || !s_isTx || !pkt)
    return false;
  // Input sampling and the main/UI loop both transmit. Serialize the ESP-IDF call
  // so their sends cannot overlap; the mutex provides priority inheritance for
  // the high-priority input task.
  if (!s_sendMutex || xSemaphoreTake(s_sendMutex, pdMS_TO_TICKS(25)) != pdTRUE)
    return false;
  pkt->seq = nextTxSeq();
  const bool ok = esp_now_send(kBroadcastMac, reinterpret_cast<const uint8_t *>(pkt), sizeof(ActuatorCmdPacket)) == ESP_OK;
  xSemaphoreGive(s_sendMutex);
  return ok;
}

ActuatorCmdPacket espnowActuatorMakePacket(ActuatorColor color, bool on) {
  ActuatorCmdPacket pkt = {};
  pkt.magic = ACTUATOR_PROTO_MAGIC;
  pkt.version = ACTUATOR_PROTO_VERSION;
  pkt.color = (uint8_t)color;
  pkt.on = on ? ACTUATOR_ON_ON : ACTUATOR_ON_OFF;
  return pkt;
}

ActuatorCmdPacket espnowActuatorMakeBubblePartyPacket(uint8_t r, uint8_t g, uint8_t b, uint8_t w, uint8_t amber) {
  ActuatorCmdPacket pkt = {};
  pkt.magic = ACTUATOR_PROTO_MAGIC;
  pkt.version = ACTUATOR_PROTO_VERSION;
  pkt.color = amber;
  pkt.on = ACTUATOR_ON_BUBBLE_PARTY;
  pkt.level_r = r;
  pkt.level_g = g;
  pkt.level_b = b;
  pkt.level_w = w;
  return pkt;
}

ActuatorCmdPacket espnowActuatorMakeBubbleKillPacket() {
  ActuatorCmdPacket pkt = {};
  pkt.magic = ACTUATOR_PROTO_MAGIC;
  pkt.version = ACTUATOR_PROTO_VERSION;
  pkt.on = ACTUATOR_ON_BUBBLE_KILL;
  return pkt;
}

ActuatorCmdPacket espnowActuatorMakeIdleDmxPacket(uint8_t r, uint8_t g, uint8_t b, uint8_t w, uint8_t amber) {
  ActuatorCmdPacket pkt = {};
  pkt.magic = ACTUATOR_PROTO_MAGIC;
  pkt.version = ACTUATOR_PROTO_VERSION;
  pkt.color = amber;
  pkt.on = ACTUATOR_ON_IDLE_DMX;
  pkt.level_r = r;
  pkt.level_g = g;
  pkt.level_b = b;
  pkt.level_w = w;
  return pkt;
}

ActuatorCmdPacket espnowActuatorMakeIdleEndPacket() {
  ActuatorCmdPacket pkt = {};
  pkt.magic = ACTUATOR_PROTO_MAGIC;
  pkt.version = ACTUATOR_PROTO_VERSION;
  pkt.on = ACTUATOR_ON_IDLE_END;
  return pkt;
}

ActuatorCmdPacket espnowActuatorMakeDirectPacket(ActuatorColor color, uint8_t targetMask, bool on) {
  ActuatorCmdPacket pkt = {};
  pkt.magic = ACTUATOR_PROTO_MAGIC;
  pkt.version = ACTUATOR_PROTO_VERSION;
  pkt.color = (uint8_t)color;
  pkt.on = ACTUATOR_ON_DIRECT;
  pkt.level_r = targetMask;
  pkt.level_g = on ? 1u : 0u;
  return pkt;
}

ActuatorCmdPacket espnowActuatorMakePurgePacket() {
  ActuatorCmdPacket pkt = {};
  pkt.magic = ACTUATOR_PROTO_MAGIC;
  pkt.version = ACTUATOR_PROTO_VERSION;
  pkt.on = ACTUATOR_ON_PURGE;
  return pkt;
}

ActuatorCmdPacket espnowActuatorMakePrimePacket(bool on) {
  ActuatorCmdPacket pkt = {};
  pkt.magic = ACTUATOR_PROTO_MAGIC;
  pkt.version = ACTUATOR_PROTO_VERSION;
  pkt.on = ACTUATOR_ON_PRIME;
  pkt.level_g = on ? 1u : 0u;
  return pkt;
}

ActuatorCmdPacket espnowActuatorMakeRgbHoldPacket(uint8_t r, uint8_t g, uint8_t b, uint8_t w, uint8_t amber) {
  ActuatorCmdPacket pkt = {};
  pkt.magic = ACTUATOR_PROTO_MAGIC;
  pkt.version = ACTUATOR_PROTO_VERSION;
  pkt.color = amber;
  pkt.on = ACTUATOR_ON_RGB_HOLD;
  pkt.level_r = r;
  pkt.level_g = g;
  pkt.level_b = b;
  pkt.level_w = w;
  return pkt;
}
