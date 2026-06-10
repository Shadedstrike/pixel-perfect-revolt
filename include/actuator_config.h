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

// Each +10s bubble fan run; re-triggered every BUBBLE_HOLD_TRIGGER_MS while any button stays held.
#ifndef BUBBLE_PARTY_MS
#define BUBBLE_PARTY_MS 10000
#endif

#ifndef BUBBLE_HOLD_TRIGGER_MS
#define BUBBLE_HOLD_TRIGGER_MS 2000
#endif

// Controller -> actuator idle DMX mirror (10 Hz); bottom-left red LED in warm-flame idle.
#ifndef ACTUATOR_IDLE_DMX_MS
#define ACTUATOR_IDLE_DMX_MS 100
#endif

#ifndef ACTUATOR_IDLE_MIRROR_LED_IDX
#define ACTUATOR_IDLE_MIRROR_LED_IDX 3
#endif

#endif
