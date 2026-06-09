#ifndef ACTUATOR_CONFIG_H
#define ACTUATOR_CONFIG_H

// WiFi channel for ESP-NOW (both boards must match).
// If the controller later joins an AP, set this to that AP's channel or call espnowActuatorSetWifiChannel().
#ifndef ESPNOW_WIFI_CHANNEL
#define ESPNOW_WIFI_CHANNEL 1
#endif

// Controller pulse width (monostable); actuator mirrors ON then OFF packets.
#ifndef ACTUATOR_PULSE_MS
#define ACTUATOR_PULSE_MS 250
#endif

#ifndef ACTUATOR_HB_MS
#define ACTUATOR_HB_MS 5000
#endif

#endif
