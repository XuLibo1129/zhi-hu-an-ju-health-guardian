set(APP_SRCS
    "app_k230_vision.c"
    "app_radar.c"
    "app_vitals_history.c"
    "app_weather.c"
    "main.c"
    "ui_font_cn_20.c"
    "app_audio_input.c"
    "app_cloud_asr.c"
    "app_cloud_context.c"
    "app_cloud_llm.c"
    "app_cloud_tts.c"
    "app_csi_receiver.c"
    "app_device_agent.c"
    "app_network.c"
    "app_voice_output.c"
)

if(CONFIG_APP_SIM800_ENABLE)
    list(APPEND APP_SRCS "app_sim800.c")
endif()

idf_component_register(
    SRCS ${APP_SRCS}
    INCLUDE_DIRS "."
    REQUIRES esp32_p4_function_ev_board esp_codec_dev lvgl heap esp_timer esp_http_client esp_http_server esp-tls json nvs_flash esp_netif esp_event esp_wifi esp_driver_uart esp_driver_gpio
)
