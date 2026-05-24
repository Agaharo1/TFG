idf_component_register(
    SRCS
        "main.c"
        "mesh_handler.c"
        "mqtt_handler.c"
        "metrics.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_wifi
        esp_netif
        nvs_flash
        esp_timer
        esp_event
        mqtt
        json
)
