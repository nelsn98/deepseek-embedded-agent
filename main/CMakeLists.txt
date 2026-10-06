idf_component_register(SRCS "app_main.c" "agent_led.c" INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_event esp_netif nvs_flash lwip esp_http_client mbedtls esp_timer)
