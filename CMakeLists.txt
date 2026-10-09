cmake_minimum_required(VERSION 3.16)

# Personal configuration lives outside the published tree, in overlay/
# (gitignored). When overlay/sdkconfig.secrets exists, the generated sdkconfig
# -- which ends up holding the Wi-Fi and MQTT credentials -- is kept in the
# overlay too, and the secrets file seeds those credentials on a fresh
# configure. Without an overlay this is a stock ESP-IDF project.
set(JK_OVERLAY "${CMAKE_CURRENT_LIST_DIR}/overlay")
if(EXISTS "${JK_OVERLAY}/sdkconfig.secrets")
    set(SDKCONFIG "${JK_OVERLAY}/sdkconfig")
    set(SDKCONFIG_DEFAULTS
        "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults"
        "${JK_OVERLAY}/sdkconfig.secrets")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(jk_bms_monitor)
