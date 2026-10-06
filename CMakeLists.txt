cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(deepseek_embedded_agent)

# Compatibility fix for led_strip 3.0.1 with ESP-IDF 6.1
idf_component_get_property(led_strip_lib espressif__led_strip COMPONENT_LIB)
target_compile_options(${led_strip_lib} PRIVATE
    "SHELL:-include esp_heap_caps.h"
)