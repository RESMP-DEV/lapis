cmake_minimum_required(VERSION 3.25)
project(lapis VERSION 0.5.0 LANGUAGES CXX)

if(NOT CMAKE_SYSTEM_NAME MATCHES "^(Darwin|Linux)$")
    message(FATAL_ERROR "lapis targets macOS and Linux")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(cmake/ProjectOptions.cmake)

option(LAPIS_BUILD_DESKTOP "Build the live desktop preview and local session service" OFF)
option(LAPIS_BUILD_SESSION "Build the existing session service and its terminal dependency" ON)
option(LAPIS_PACKAGE "Build the desktop as the downloadable app (see scripts/package_macos.py)" OFF)
option(LAPIS_BUILD_SUPERVISOR_STATE "Build the focused supervisor state-core test target" OFF)
option(LAPIS_BUILD_SUPERVISOR_RUNTIME "Build the experimental one-session supervisor runtime" OFF)

if(LAPIS_BUILD_DESKTOP AND NOT LAPIS_BUILD_SESSION)
    message(FATAL_ERROR "LAPIS_BUILD_DESKTOP requires LAPIS_BUILD_SESSION")
endif()

include(CTest)

if(LAPIS_BUILD_SESSION)
    add_subdirectory(adapters/codex)
    add_subdirectory(adapters/claude)
    add_subdirectory(services/session)
    target_link_libraries(lapis_session_service PRIVATE lapis_codex_adapter lapis_claude_adapter)
endif()

if(LAPIS_BUILD_SUPERVISOR_RUNTIME AND NOT LAPIS_BUILD_SUPERVISOR_STATE)
    set(LAPIS_BUILD_SUPERVISOR_STATE ON)
endif()

if(LAPIS_BUILD_SUPERVISOR_STATE)
    add_subdirectory(services/supervisor)
endif()

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
    add_subdirectory(apps/desktop)
endif()
