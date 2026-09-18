cmake_minimum_required(VERSION 3.25)
project(lapis_ghostty_probe LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
get_filename_component(LAPIS_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
include("${LAPIS_SOURCE_DIR}/cmake/ProjectOptions.cmake")

set(GHOSTTY_PREFIX "" CACHE PATH "Prefix of the separately built pinned libghostty-vt")
if(NOT EXISTS "${GHOSTTY_PREFIX}/include/ghostty/vt.h" OR
   NOT EXISTS "${GHOSTTY_PREFIX}/lib/libghostty-vt.a")
    message(FATAL_ERROR "Set GHOSTTY_PREFIX to the pinned headless Ghostty build prefix")
endif()

add_library(ghostty_vt STATIC IMPORTED)
set_target_properties(ghostty_vt PROPERTIES
    IMPORTED_LOCATION "${GHOSTTY_PREFIX}/lib/libghostty-vt.a"
    INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${GHOSTTY_PREFIX}/include"
    INTERFACE_INCLUDE_DIRECTORIES "${GHOSTTY_PREFIX}/include")
add_executable(lapis_ghostty_probe adapter.cpp ../replay.cpp)
target_link_libraries(lapis_ghostty_probe PRIVATE lapis_project_options ghostty_vt)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(lapis_ghostty_probe PRIVATE m pthread dl)
endif()
enable_testing()
add_test(NAME ghostty_replay COMMAND lapis_ghostty_probe)
set_tests_properties(ghostty_replay PROPERTIES TIMEOUT 30)
