#ifndef ACTUATOR_PROTOCOL_H
#define ACTUATOR_PROTOCOL_H

#include <stdint.h>

// ESP-NOW payload between controller and actuator node (solenoids + DMX).
#define ACTUATOR_PROTO_MAGIC 0xA7u
#define ACTUATOR_PROTO_VERSION 3u

#define ACTUATOR_ON_OFF 0u
#define ACTUATOR_ON_ON 1u
#define ACTUATOR_ON_BUBBLE_PARTY 2u
#define ACTUATOR_ON_BUBBLE_KILL 3u
#define ACTUATOR_ON_IDLE_DMX 4u
#define ACTUATOR_ON_IDLE_END 5u
#define ACTUATOR_ON_RGB_HOLD 6u

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
  uint8_t color; // color hold cmd; PAR amber level for bubble party
  uint8_t on;    // 0=off 1=on 2=bubble party (20s on actuator)
  uint32_t seq;
  uint8_t level_r;
  uint8_t level_g;
  uint8_t level_b;
  uint8_t level_w;
};

static inline bool actuatorPacketValid(const ActuatorCmdPacket *p) {
  if (!p || p->magic != ACTUATOR_PROTO_MAGIC || p->version != ACTUATOR_PROTO_VERSION)
    return false;
  if (p->on == ACTUATOR_ON_BUBBLE_PARTY || p->on == ACTUATOR_ON_BUBBLE_KILL || p->on == ACTUATOR_ON_IDLE_DMX ||
      p->on == ACTUATOR_ON_IDLE_END || p->on == ACTUATOR_ON_RGB_HOLD)
    return true;
  return p->color < ACTUATOR_COLOR_COUNT && (p->on == ACTUATOR_ON_OFF || p->on == ACTUATOR_ON_ON);
}

#endif
