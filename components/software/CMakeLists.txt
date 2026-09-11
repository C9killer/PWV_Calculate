# Manage all middleware modules as one ESP-IDF component.
# Run idf.py reconfigure after adding or removing source/header files.
# Keep tests and examples outside this tree: all matching sources are built.
file(GLOB_RECURSE software_srcs
    "${CMAKE_CURRENT_LIST_DIR}/*.c"
    "${CMAKE_CURRENT_LIST_DIR}/*.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/*.cc"
    "${CMAKE_CURRENT_LIST_DIR}/*.S"
)

file(GLOB_RECURSE software_headers
    "${CMAKE_CURRENT_LIST_DIR}/*.h"
    "${CMAKE_CURRENT_LIST_DIR}/*.hpp"
)
set(software_include_dirs ".")
foreach(header IN LISTS software_headers)
    get_filename_component(header_dir "${header}" DIRECTORY)
    list(APPEND software_include_dirs "${header_dir}")
endforeach()
list(REMOVE_DUPLICATES software_include_dirs)

idf_component_register(
    SRCS ${software_srcs}
    INCLUDE_DIRS ${software_include_dirs}
    REQUIRES hardware esp_wifi esp_event esp_netif
)
