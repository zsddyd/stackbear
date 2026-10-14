idf_component_register(
    SRCS "main.c"
         "sb_config.c"
         "sb_diag.c"
         "sb_proto.c"
         "sb_vad.c"
         "sb_opus.c"
         "sb_net.c"
         "sb_net_web.c"
         "sb_uplink.c"
         "sb_recorder.c"
         "sb_ui.c"
    INCLUDE_DIRS "."
    REQUIRES bsp esp_event esp_http_server esp_hw_support esp_netif esp_timer esp_wifi lwip nvs_flash
)
