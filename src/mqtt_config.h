#ifndef MQTT_CONFIG_H
#define MQTT_CONFIG_H

// Network + broker for controller (main firmware) and solenoid-node (WiFi builds).
//
// Home Assistant: http://10.0.0.231:8123/... is the web UI. MQTT clients must use the
// Mosquitto broker host + port (default 1883 on the same machine), e.g. 10.0.0.231:1883.
// In HA: Settings → Add-ons → Mosquitto broker → Info (or Documentation) for login if required.

// ---- Physical link (LILYGO T-ETH uses W5500, not WiFi) ----
// 0 = WiFi station (WIFI_SSID / WIFI_PASSWORD below).
// 1 = T-ETH-Elite ESP32-S3 (W5500 on SPI; pins match LilyGO examples/MQTTClient + utilities.h).
// 2 = T-ETH-Lite ESP32-S3 (W5500).
#ifndef LILYGO_ETH_BOARD
#define LILYGO_ETH_BOARD 0
#endif

#ifndef WIFI_SSID
#define WIFI_SSID "A Wee Lassie"
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD "ThereIsNoLight"
#endif

#ifndef MQTT_HOST
#define MQTT_HOST "10.0.0.231"
#endif
#ifndef MQTT_PORT
#define MQTT_PORT 1883
#endif

// HA Mosquitto add-on → Logins (plain MQTT on 1883; ESP firmware uses non-TLS PubSubClient).
#ifndef MQTT_USER
#define MQTT_USER "mqtt"
#endif
#ifndef MQTT_PASSWORD
#define MQTT_PASSWORD "fuckyoumqtt"
#endif

// Controller: each side-column press sends solenoid ON then OFF after this many ms (monostable; tune for slow coils).
#ifndef MQTT_SOLENOID_PULSE_MS
#define MQTT_SOLENOID_PULSE_MS 250
#endif

#endif
