# Manage all driver modules (LED/, KEY/, etc.) as one ESP-IDF component.
# Run idf.py reconfigure after adding or removing source/header files.
# Keep tests and examples outside this tree: all matching sources are built.
file(GLOB_RECURSE hardware_srcs
    "${CMAKE_CURRENT_LIST_DIR}/*.c"
    "${CMAKE_CURRENT_LIST_DIR}/*.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/*.cc"
    "${CMAKE_CURRENT_LIST_DIR}/*.S"
)

file(GLOB_RECURSE hardware_headers
    "${CMAKE_CURRENT_LIST_DIR}/*.h"
    "${CMAKE_CURRENT_LIST_DIR}/*.hpp"
)
set(hardware_include_dirs ".")
foreach(header IN LISTS hardware_headers)
    get_filename_component(header_dir "${header}" DIRECTORY)
    list(APPEND hardware_include_dirs "${header_dir}")
endforeach()
list(REMOVE_DUPLICATES hardware_include_dirs)

idf_component_register(
    SRCS ${hardware_srcs}
    INCLUDE_DIRS ${hardware_include_dirs}
    # Add ESP-IDF dependencies here as drivers are implemented, for example:
    # REQUIRES esp_driver_gpio esp_driver_spi esp_driver_i2c
    REQUIRES esp_driver_gpio
    PRIV_REQUIRES espressif__led_strip
)
