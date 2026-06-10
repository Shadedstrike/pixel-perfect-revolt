#ifndef ACTUATOR_CONFIG_H
#define ACTUATOR_CONFIG_H

// WiFi channel for ESP-NOW (both boards must match).
// If the controller later joins an AP, set this to that AP's channel or call espnowActuatorSetWifiChannel().
#ifndef ESPNOW_WIFI_CHANNEL
#define ESPNOW_WIFI_CHANNEL 1
#endif

// Legacy pulse width (unused — side buttons now hold ON while pressed).
#ifndef ACTUATOR_PULSE_MS
#define ACTUATOR_PULSE_MS 250
#endif

#ifndef ACTUATOR_HB_MS
#define ACTUATOR_HB_MS 5000
#endif

// One 2s bubble fan burst per hold (re-arm after all buttons released).
#ifndef BUBBLE_PARTY_MS
#define BUBBLE_PARTY_MS 2000
#endif

#ifndef BUBBLE_HOLD_TRIGGER_MS
#define BUBBLE_HOLD_TRIGGER_MS 2000
#endif

// Bubble 1CH: soft-start / soft-stop to reduce inrush current (manual: 10-255 = variable wind).
#ifndef BUBBLE_FAN_RAMP_UP_MS
#define BUBBLE_FAN_RAMP_UP_MS 1500
#endif
#ifndef BUBBLE_FAN_RAMP_DOWN_MS
#define BUBBLE_FAN_RAMP_DOWN_MS 500
#endif

// Controller -> actuator idle keepalive (10 Hz); actuator runs local PAR+bubble animation.
#ifndef ACTUATOR_IDLE_DMX_MS
#define ACTUATOR_IDLE_DMX_MS 100
#endif

#ifndef ACTUATOR_RGB_SYNC_MS
#define ACTUATOR_RGB_SYNC_MS 50
#endif

// Actuator: if outputs are on and no ESP-NOW for this long, force everything off.
#ifndef ACTUATOR_RX_FAILSAFE_MS
#define ACTUATOR_RX_FAILSAFE_MS 3000
#endif

#endif
