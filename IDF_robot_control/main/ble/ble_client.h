#pragma once

#include "config.h"


typedef void (*BleMessageHandler)(int, const uint8_t*);
void SetBleMessageHandler(BleMessageHandler fcn);

void ObdBleClientSetup();
