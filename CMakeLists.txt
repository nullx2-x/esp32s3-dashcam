cmake_minimum_required(VERSION 3.16)

# Local, untracked overrides (Wi-Fi credentials, AP password).
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults.local")
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.defaults.local")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32s3_dashcam)
