#ifndef MQTT_PROTOCOL_H
#define MQTT_PROTOCOL_H

// Shared between controller and solenoid actuator firmware.
// Payload: "<color>,<0|1>" e.g. "red,1" press, "red,0" release.
#define MQTT_TOPIC_SOLENOID_CMD "pyrrisma/solenoid/cmd"

#endif
