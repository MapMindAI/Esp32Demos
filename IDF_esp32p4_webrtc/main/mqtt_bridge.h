#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*mqtt_bridge_cmd_handler_t)(const char *cmd, void *ctx);

int mqtt_bridge_start(void);
void mqtt_bridge_stop(void);
void mqtt_bridge_set_cmd_handler(mqtt_bridge_cmd_handler_t handler, void *ctx);
int mqtt_bridge_publish_json(const char *topic_suffix, const char *json);
bool mqtt_bridge_is_connected(void);

#ifdef __cplusplus
}
#endif
