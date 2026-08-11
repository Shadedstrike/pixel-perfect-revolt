#include "espnow_actuator.h"
#include "actuator_config.h"
#include "serial_status.h"

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

static bool s_ready = false;
static bool s_isTx = false;
static EspnowActuatorRecvFn s_recvFn = nullptr;
static uint32_t s_txSeq = 0;
static uint32_t s_txOk = 0;
static uint32_t s_txFail = 0;
static uint32_t s_rxBad = 0;

static const uint8_t kBroadcastMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

static void formatMac(char *out, size_t outLen, const uint8_t mac[6]) {
  snprintf(out, outLen, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void onEspnowSend(const uint8_t *mac, esp_now_send_status_t status) {
  char macStr[18];
  formatMac(macStr, sizeof(macStr), mac);
  if (status == ESP_NOW_SEND_SUCCESS) {
    ++s_txOk;
    Serial.printf("[ESPNOW] send_cb OK mac=%s  ok=%u fail=%u\n", macStr, (unsigned)s_txOk, (unsigned)s_txFail);
  } else {
    ++s_txFail;
    Serial.printf("[ESPNOW] send_cb FAIL mac=%s  ok=%u fail=%u\n", macStr, (unsigned)s_txOk, (unsigned)s_txFail);
  }
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

bool espnowActuatorSend(const ActuatorCmdPacket *pkt) {
  if (!s_ready || !s_isTx || !pkt)
    return false;
  return esp_now_send(kBroadcastMac, reinterpret_cast<const uint8_t *>(pkt), sizeof(ActuatorCmdPacket)) == ESP_OK;
}

ActuatorCmdPacket espnowActuatorMakePacket(ActuatorColor color, bool on) {
  ActuatorCmdPacket pkt = {};
  pkt.magic = ACTUATOR_PROTO_MAGIC;
  pkt.version = ACTUATOR_PROTO_VERSION;
  pkt.color = (uint8_t)color;
  pkt.on = on ? ACTUATOR_ON_ON : ACTUATOR_ON_OFF;
  pkt.seq = ++s_txSeq;
  return pkt;
}

ActuatorCmdPacket espnowActuatorMakeBubblePartyPacket(uint8_t r, uint8_t g, uint8_t b, uint8_t w, uint8_t amber) {
  ActuatorCmdPacket pkt = {};
  pkt.magic = ACTUATOR_PROTO_MAGIC;
  pkt.version = ACTUATOR_PROTO_VERSION;
  pkt.color = amber;
  pkt.on = ACTUATOR_ON_BUBBLE_PARTY;
  pkt.seq = ++s_txSeq;
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
  pkt.seq = ++s_txSeq;
  return pkt;
}

ActuatorCmdPacket espnowActuatorMakeIdleDmxPacket(uint8_t r, uint8_t g, uint8_t b, uint8_t w, uint8_t amber) {
  ActuatorCmdPacket pkt = {};
  pkt.magic = ACTUATOR_PROTO_MAGIC;
  pkt.version = ACTUATOR_PROTO_VERSION;
  pkt.color = amber;
  pkt.on = ACTUATOR_ON_IDLE_DMX;
  pkt.seq = ++s_txSeq;
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
  pkt.seq = ++s_txSeq;
  return pkt;
}

ActuatorCmdPacket espnowActuatorMakeDirectPacket(ActuatorColor color, uint8_t targetMask, bool on) {
  ActuatorCmdPacket pkt = {};
  pkt.magic = ACTUATOR_PROTO_MAGIC;
  pkt.version = ACTUATOR_PROTO_VERSION;
  pkt.color = (uint8_t)color;
  pkt.on = ACTUATOR_ON_DIRECT;
  pkt.seq = ++s_txSeq;
  pkt.level_r = targetMask;
  pkt.level_g = on ? 1u : 0u;
  return pkt;
}

ActuatorCmdPacket espnowActuatorMakeRgbHoldPacket(uint8_t r, uint8_t g, uint8_t b, uint8_t w, uint8_t amber) {
  ActuatorCmdPacket pkt = {};
  pkt.magic = ACTUATOR_PROTO_MAGIC;
  pkt.version = ACTUATOR_PROTO_VERSION;
  pkt.color = amber;
  pkt.on = ACTUATOR_ON_RGB_HOLD;
  pkt.seq = ++s_txSeq;
  pkt.level_r = r;
  pkt.level_g = g;
  pkt.level_b = b;
  pkt.level_w = w;
  return pkt;
}
