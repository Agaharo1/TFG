set(srcs "main.c" "wifi.c" "mqtt.c"
        )

idf_component_register(SRCS "${srcs}"
                    INCLUDE_DIRS  ".")
