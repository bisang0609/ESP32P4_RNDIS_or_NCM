cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/components"
    "${CMAKE_CURRENT_LIST_DIR}/managed_components"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(usb_ncm_ytmd_test)
