idf_component_register(
SRCS
     "./main.c"
     "./ble/ble_client.c"
     "./control/control_callback.c"

INCLUDE_DIRS
     "."

REQUIRES bt driver nvs_flash
)
