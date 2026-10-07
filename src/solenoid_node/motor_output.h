#ifndef SOLENOID_NODE_MOTOR_OUTPUT_H
#define SOLENOID_NODE_MOTOR_OUTPUT_H

#include <Arduino.h>
#include "actuator_protocol.h"

bool motorOutputBegin();
void motorOutputEarlyInit();
void motorOutputSetColor(ActuatorColor color, bool on);
void motorOutputSetColorReverse(ActuatorColor color);
void motorOutputAllReverse();
void motorOutputAllForward();
void motorOutputAllOff();
bool motorOutputHasMotor(ActuatorColor color);
bool motorOutputAnyOn();
bool motorOutputStbyEnabled();
int motorOutputStbyLevel();

#endif
