#ifndef ACTUATOR_PROTOCOL_H
#define ACTUATOR_PROTOCOL_H

#include <stdint.h>

// ESP-NOW payload between controller and actuator node (solenoids + DMX).
#define ACTUATOR_PROTO_MAGIC 0xA7u
#define ACTUATOR_PROTO_VERSION 1u

enum ActuatorColor : uint8_t {
  ACTUATOR_COLOR_RED = 0,
  ACTUATOR_COLOR_GREEN = 1,
  ACTUATOR_COLOR_BLUE = 2,
  ACTUATOR_COLOR_YELLOW = 3,
  ACTUATOR_COLOR_COUNT = 4,
};

struct __attribute__((packed)) ActuatorCmdPacket {
  uint8_t magic;
  uint8_t version;
  uint8_t color;
  uint8_t on; // 0 = off, non-zero = on
  uint32_t seq;
};

static inline bool actuatorPacketValid(const ActuatorCmdPacket *p) {
  return p && p->magic == ACTUATOR_PROTO_MAGIC && p->version == ACTUATOR_PROTO_VERSION &&
         p->color < ACTUATOR_COLOR_COUNT && (p->on == 0 || p->on == 1);
}

#endif
