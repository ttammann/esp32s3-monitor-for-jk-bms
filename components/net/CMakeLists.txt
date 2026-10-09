idf_component_register(SRCS "wifi.c" "mqtt.c" "ha_discovery.c"
                       INCLUDE_DIRS "include"
                       REQUIRES util esp_wifi esp_event esp_netif mqtt)
