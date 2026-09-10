idf_component_register(SRCS "main.c"
                        PRIV_REQUIRES esp_wifi nvs_flash driver bt esp_netif protocomm
                        INCLUDE_DIRS ".")