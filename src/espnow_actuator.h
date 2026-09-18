#ifndef ESPNOW_ACTUATOR_H
#define ESPNOW_ACTUATOR_H

#include <Arduino.h>
#include "actuator_protocol.h"

typedef void (*EspnowActuatorRecvFn)(const ActuatorCmdPacket *pkt, const uint8_t mac[6]);

bool espnowActuatorBeginTx();
bool espnowActuatorBeginRx(EspnowActuatorRecvFn onCmd);

// Match receiver to controller AP channel when known (call after WiFi connected).
void espnowActuatorSetWifiChannel(uint8_t channel);

bool espnowActuatorSend(const ActuatorCmdPacket *pkt);
bool espnowActuatorReady();

ActuatorCmdPacket espnowActuatorMakePacket(ActuatorColor color, bool on);
ActuatorCmdPacket espnowActuatorMakeBubblePartyPacket(uint8_t r, uint8_t g, uint8_t b, uint8_t w, uint8_t amber);
ActuatorCmdPacket espnowActuatorMakeBubbleKillPacket();
ActuatorCmdPacket espnowActuatorMakeIdleDmxPacket(uint8_t r, uint8_t g, uint8_t b, uint8_t w, uint8_t amber);
ActuatorCmdPacket espnowActuatorMakeIdleEndPacket();
ActuatorCmdPacket espnowActuatorMakeRgbHoldPacket(uint8_t r, uint8_t g, uint8_t b, uint8_t w, uint8_t amber);
// Independent solenoid / motor / relay control for one colour (ACTUATOR_ON_DIRECT).
ActuatorCmdPacket espnowActuatorMakeDirectPacket(ActuatorColor color, uint8_t targetMask, bool on);
ActuatorCmdPacket espnowActuatorMakePurgePacket();

#endif
