cmake_minimum_required(VERSION 3.16)

# Pull in the artnet component from this repository plus the shared Wi-Fi helper.
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../../../components"
    "${CMAKE_CURRENT_LIST_DIR}/../common"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(artnet_transmit)
