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
