idf_component_register(SRCS "rest_api_main.c" "net.c" "display.c" "audio.c" "font.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_http_server esp_wifi nvs_flash esp_netif json
                                spiffs esp_lcd esp_driver_spi esp_driver_i2c esp_driver_i2s)

# Bake WiFi/API credentials from the (gitignored) .env into a generated header.
# Runs at configure time; editing .env re-triggers configure via CONFIGURE_DEPENDS.
set(_env "${CMAKE_CURRENT_SOURCE_DIR}/../.env")
set(_hdr "${CMAKE_CURRENT_SOURCE_DIR}/wifi_credentials.h")
set_property(DIRECTORY "${CMAKE_SOURCE_DIR}" APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_env}")
execute_process(
    COMMAND python3 "${CMAKE_CURRENT_SOURCE_DIR}/../tools/gen_credentials.py" "${_env}" "${_hdr}"
    RESULT_VARIABLE _cred_res)
if(NOT _cred_res EQUAL 0)
    message(FATAL_ERROR "gen_credentials.py failed — is apps/rest_api/.env present and valid?")
endif()
