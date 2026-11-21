cmake_minimum_required(VERSION 3.16)
# one version for firmware, agent and installer: the repo's VERSION file
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/../VERSION" PROJECT_VER LIMIT_COUNT 1)

# The board to build for: a directory under boards/ (see boards/README.md).
# `make build BOARD=<id>` keeps each board's build and sdkconfig apart.
set(BOARD "waveshare-esp32c6-amoled-2.16" CACHE STRING "board directory under firmware/boards")
set(BOARD_DIR "${CMAKE_CURRENT_LIST_DIR}/boards/${BOARD}")
if(NOT EXISTS "${BOARD_DIR}/board/CMakeLists.txt")
    message(FATAL_ERROR "unknown BOARD '${BOARD}': there's no ${BOARD_DIR}/board")
endif()
set(EXTRA_COMPONENT_DIRS "${BOARD_DIR}")
set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults;${BOARD_DIR}/sdkconfig.defaults")

# The board decides the chip (CONFIG_IDF_TARGET in its sdkconfig.defaults). A build
# directory made for another chip can't be reused.
file(STRINGS "${BOARD_DIR}/sdkconfig.defaults" _target REGEX "^CONFIG_IDF_TARGET=")
string(REGEX REPLACE "^CONFIG_IDF_TARGET=\"?([a-z0-9]+)\"?$" "\\1" BOARD_TARGET "${_target}")
if(NOT BOARD_TARGET)
    message(FATAL_ERROR "${BOARD_DIR}/sdkconfig.defaults must set CONFIG_IDF_TARGET")
endif()
if(IDF_TARGET AND NOT IDF_TARGET STREQUAL BOARD_TARGET)
    message(FATAL_ERROR "BOARD ${BOARD} is an ${BOARD_TARGET} but this build is for ${IDF_TARGET}: "
                        "build it in its own directory (make build BOARD=${BOARD})")
endif()
set(IDF_TARGET "${BOARD_TARGET}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(DEPENDENCIES_LOCK "${BOARD_DIR}/dependencies.lock")   # one per board/chip
project(sparkesp32)
