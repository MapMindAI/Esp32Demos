idf_component_register(
SRCS
     "./main.c"
     "./ble/ble_client.c"
     "./control/control_callback.c"
     "./mcp2515/mcp2515.c"
     "./mcp2515/mcp2515_canbus.c"

INCLUDE_DIRS
     "."

REQUIRES bt driver nvs_flash
)
