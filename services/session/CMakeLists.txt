# Internal POSIX resource ownership. Engine and desktop code must not include it.
add_library(lapis_session_platform STATIC
    src/platform/posix/unique_fd.cpp)
target_link_libraries(lapis_session_platform PRIVATE lapis_project_options)
target_include_directories(lapis_session_platform PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")

if(BUILD_TESTING)
    add_executable(lapis_session_platform_tests tests/unique_fd_test.cpp)
    target_link_libraries(lapis_session_platform_tests PRIVATE
        lapis_project_options lapis_session_platform)
    add_test(NAME session-platform-ownership COMMAND lapis_session_platform_tests)
    set_tests_properties(session-platform-ownership PROPERTIES TIMEOUT 10)
endif()

# The production adapter is part of every normal build. Bootstrap its pinned
# external dependency once; do not silently omit behavior when it is missing.
include("${PROJECT_SOURCE_DIR}/cmake/Ghostty.cmake")
add_library(lapis_terminal STATIC src/terminal/terminal.cpp)
target_link_libraries(lapis_terminal PRIVATE lapis_project_options lapis_ghostty_vt)
target_include_directories(lapis_terminal PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

if(BUILD_TESTING)
    add_executable(lapis_terminal_tests tests/terminal/terminal_test.cpp)
    target_link_libraries(lapis_terminal_tests PRIVATE lapis_project_options lapis_terminal)
    add_test(NAME terminal-behavior COMMAND lapis_terminal_tests)
    set_tests_properties(terminal-behavior PROPERTIES TIMEOUT 30)
endif()

if(LAPIS_BUILD_DESKTOP)
    find_package(Qt6 6.11.2 EXACT REQUIRED COMPONENTS Core Network)
    set(CMAKE_AUTOMOC ON)
    add_library(lapis_local_protocol STATIC src/transport/local_protocol.cpp)
    target_include_directories(lapis_local_protocol PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(lapis_local_protocol PRIVATE lapis_project_options PUBLIC lapis_terminal Qt6::Core)
    add_library(lapis_pty_process STATIC src/platform/posix/pty_process.cpp src/platform/posix/pty_process.hpp)
    target_link_libraries(lapis_pty_process PRIVATE lapis_project_options PUBLIC lapis_session_platform lapis_terminal Qt6::Core)
    add_executable(lapis_session_service src/session_service.cpp)
    target_link_libraries(lapis_session_service PRIVATE lapis_project_options lapis_pty_process lapis_local_protocol Qt6::Network)
    if(BUILD_TESTING)
        add_executable(lapis_local_protocol_tests tests/local_protocol_test.cpp)
        target_link_libraries(lapis_local_protocol_tests PRIVATE lapis_project_options lapis_local_protocol)
        add_test(NAME local-protocol COMMAND lapis_local_protocol_tests)
        set_tests_properties(local-protocol PROPERTIES TIMEOUT 10)
        add_executable(lapis_pty_process_tests tests/pty_process_test.cpp)
        target_link_libraries(lapis_pty_process_tests PRIVATE lapis_project_options lapis_pty_process)
        add_test(NAME pty-process COMMAND lapis_pty_process_tests)
        set_tests_properties(pty-process PROPERTIES TIMEOUT 10)
    endif()
endif()
