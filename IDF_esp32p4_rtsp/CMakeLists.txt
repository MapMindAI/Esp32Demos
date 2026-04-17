# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

message("build IDF_esp32p4_rtsp")

set(SDKCONFIG_DEFAULTS "sdkconfig.defaults")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(IDF_esp32p4_rtsp)
