idf_component_register(SRCS app.cpp sensor.cpp relay.cpp ui.cpp storage.cpp network.cpp network_settings.cpp web.cpp web_access.cpp analysis_runtime.cpp
  "${CMAKE_CURRENT_LIST_DIR}/../../common/core.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/../../common/analysis.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/../../common/lcd.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/../../common/network_config.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/../../common/mqtt_discovery.cpp"
  INCLUDE_DIRS "." "${CMAKE_CURRENT_LIST_DIR}/../../common"
  REQUIRES driver esp_timer esp_system nvs_flash esp_lcd esp_wifi esp_event esp_netif mqtt json esp_http_server app_update mbedtls bootloader_support
  EMBED_TXTFILES "web/index.html" "web/logo.svg")
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Wno-unused-parameter)
