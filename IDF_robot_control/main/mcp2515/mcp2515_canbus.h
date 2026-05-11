#pragma once

#include <stdint.h>

typedef void (*Mcp2515CanbusMessageHandler)(int len, const uint8_t* data);

void Mcp2515CanbusSetMessageHandler(Mcp2515CanbusMessageHandler handler);
void Mcp2515CanbusInit(void);
