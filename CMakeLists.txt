# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

set(CMAKE_C_USE_RESPONSE_FILE_FOR_OBJECTS 1)
set(CMAKE_CXX_USE_RESPONSE_FILE_FOR_OBJECTS 1)
set(CMAKE_NINJA_FORCE_RESPONSE_FILE 1)

# Add this line to disable the specific warning
add_compile_options(-Wno-missing-field-initializers)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
idf_build_set_property(MINIMAL_BUILD ON)

set(FIRMWARE_VERSION_FILE "${CMAKE_CURRENT_LIST_DIR}/firmware_version.json")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${FIRMWARE_VERSION_FILE}")
file(READ "${FIRMWARE_VERSION_FILE}" FIRMWARE_VERSION_JSON)
foreach(FIELD major stable final beta)
    string(REGEX MATCH "\"${FIELD}\"[ \\t]*:[ \\t]*([0-9]+)" _firmware_version_match "${FIRMWARE_VERSION_JSON}")
    if(NOT _firmware_version_match)
        message(FATAL_ERROR "Missing numeric '${FIELD}' in ${FIRMWARE_VERSION_FILE}")
    endif()
    set(FIRMWARE_VERSION_${FIELD} "${CMAKE_MATCH_1}")
endforeach()
string(REGEX MATCH "\"channel\"[ \\t]*:[ \\t]*\"([^\"]+)\"" _firmware_channel_match "${FIRMWARE_VERSION_JSON}")
if(NOT _firmware_channel_match)
    message(FATAL_ERROR "Missing channel in ${FIRMWARE_VERSION_FILE}")
endif()
set(FIRMWARE_VERSION_CHANNEL "${CMAKE_MATCH_1}")
set(PROJECT_VER "${FIRMWARE_VERSION_major}.${FIRMWARE_VERSION_stable}.${FIRMWARE_VERSION_final}")
if(NOT FIRMWARE_VERSION_CHANNEL STREQUAL "stable")
    string(APPEND PROJECT_VER "_beta_${FIRMWARE_VERSION_CHANNEL}_${FIRMWARE_VERSION_beta}")
endif()
project(xiaozhi)
