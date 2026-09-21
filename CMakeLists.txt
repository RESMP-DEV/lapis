cmake_minimum_required(VERSION 3.25)
project(lapis VERSION 0.1.0 LANGUAGES CXX)

if(NOT CMAKE_SYSTEM_NAME MATCHES "^(Darwin|Linux)$")
    message(FATAL_ERROR "lapis targets macOS and Linux")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(cmake/ProjectOptions.cmake)

option(LAPIS_BUILD_DESKTOP "Build the live desktop preview and local session service" OFF)

include(CTest)
add_subdirectory(services/session)

if(BUILD_TESTING)
    # Keep the dependency-free replay contract checked as C++20. Engine adapters
    # are built and exercised separately by scripts/probe_terminal.py.
    add_library(lapis_terminal_probe_contract OBJECT tools/terminal_probe/replay.cpp)
    target_link_libraries(lapis_terminal_probe_contract PRIVATE lapis_project_options)
    find_package(Threads REQUIRED)
    add_executable(lapis_toolchain_smoke tools/verification/smoke.cpp)
    target_link_libraries(lapis_toolchain_smoke PRIVATE lapis_project_options Threads::Threads)
    add_test(NAME toolchain-smoke COMMAND lapis_toolchain_smoke)
    set_tests_properties(toolchain-smoke PROPERTIES TIMEOUT 10)
endif()

if(LAPIS_BUILD_DESKTOP)
    add_subdirectory(adapters/codex)
    target_link_libraries(lapis_session_service PRIVATE lapis_codex_adapter)
    add_subdirectory(apps/desktop)
endif()
