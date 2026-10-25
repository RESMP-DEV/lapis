find_package(Qt6 6.11.2 EXACT REQUIRED COMPONENTS Core Network)

# The focused state-only build reuses the session platform RAII source without
# configuring Ghostty or the whole session service. A normal session build
# supplies the same target above this component.
if(NOT TARGET lapis_session_platform)
    add_library(lapis_session_platform STATIC
        "${CMAKE_CURRENT_SOURCE_DIR}/../session/src/platform/posix/unique_fd.cpp")
    target_include_directories(lapis_session_platform
        PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../session/src")
    target_link_libraries(lapis_session_platform PRIVATE lapis_project_options)
endif()

add_library(lapis_supervisor_state STATIC
    src/state.cpp src/json_state_codec.cpp src/posix_state_storage.cpp)
target_include_directories(lapis_supervisor_state
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(lapis_supervisor_state
    PRIVATE lapis_project_options lapis_session_platform
    PUBLIC Qt6::Core)

if(BUILD_TESTING)
    add_executable(lapis_supervisor_state_tests tests/supervisor_state_test.cpp)
    target_link_libraries(lapis_supervisor_state_tests PRIVATE
        lapis_project_options lapis_supervisor_state)
    add_test(NAME supervisor-state COMMAND lapis_supervisor_state_tests)
    set_tests_properties(supervisor-state PROPERTIES TIMEOUT 10)
endif()

if(LAPIS_BUILD_SUPERVISOR_RUNTIME)
    add_library(lapis_supervisor_runtime STATIC
        src/birth.cpp src/client.cpp src/runtime.cpp src/control_server.cpp)
    target_include_directories(lapis_supervisor_runtime
        PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_link_libraries(lapis_supervisor_runtime
        PRIVATE lapis_project_options lapis_session_platform lapis_session_launch
        PUBLIC lapis_supervisor_state Qt6::Core Qt6::Network)
    if(TARGET lapis_local_protocol)
        target_sources(lapis_supervisor_runtime PRIVATE src/session_service_launcher.cpp)
        target_link_libraries(lapis_supervisor_runtime PRIVATE
            lapis_local_protocol lapis_session_launch Qt6::Network)
    else()
        message(FATAL_ERROR
            "LAPIS_BUILD_SUPERVISOR_RUNTIME requires LAPIS_BUILD_SESSION")
    endif()
    add_executable(lapis_supervisor_child_fixture tests/supervisor_child_fixture.cpp)
    target_link_libraries(lapis_supervisor_child_fixture PRIVATE lapis_project_options)
    add_executable(lapis_supervisor_daemon src/daemon.cpp)
    target_include_directories(lapis_supervisor_daemon PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../session/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/../session/include")
    target_link_libraries(lapis_supervisor_daemon PRIVATE
        lapis_project_options lapis_supervisor_runtime Qt6::Core Qt6::Network)
    target_compile_definitions(lapis_supervisor_daemon PRIVATE
        LAPIS_SUPERVISOR_SESSION_SERVICE="$<TARGET_FILE:lapis_session_service>")
    add_dependencies(lapis_supervisor_daemon lapis_session_service)
    add_executable(lapis_supervisor src/cli.cpp)
    set_target_properties(lapis_supervisor PROPERTIES OUTPUT_NAME lapis)
    target_link_libraries(lapis_supervisor PRIVATE
        lapis_project_options lapis_supervisor_runtime Qt6::Core Qt6::Network)
    if(APPLE)
        if(TARGET Qt6::QAppleNetworkInformationPlugin)
            add_dependencies(lapis_supervisor Qt6::QAppleNetworkInformationPlugin)
            add_custom_command(TARGET lapis_supervisor POST_BUILD
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "$<TARGET_FILE:Qt6::QAppleNetworkInformationPlugin>"
                    "$<TARGET_FILE_DIR:lapis_supervisor>/plugins/networkinformation/$<TARGET_FILE_NAME:Qt6::QAppleNetworkInformationPlugin>"
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "${CMAKE_CURRENT_SOURCE_DIR}/lapis_supervisor.qt.conf"
                    "$<TARGET_FILE_DIR:lapis_supervisor>/qt.conf"
                VERBATIM)
        else()
            message(FATAL_ERROR
                "The macOS supervisor client requires Qt6::QAppleNetworkInformationPlugin")
        endif()
    endif()

    if(BUILD_TESTING)
        add_executable(lapis_supervisor_runtime_tests tests/supervisor_runtime_test.cpp)
        target_link_libraries(lapis_supervisor_runtime_tests PRIVATE
            lapis_project_options lapis_supervisor_runtime)
        target_compile_definitions(lapis_supervisor_runtime_tests PRIVATE
            LAPIS_SUPERVISOR_CHILD_FIXTURE="$<TARGET_FILE:lapis_supervisor_child_fixture>")
        add_dependencies(lapis_supervisor_runtime_tests lapis_supervisor_child_fixture)
        add_test(NAME supervisor-runtime COMMAND lapis_supervisor_runtime_tests)
        set_tests_properties(supervisor-runtime PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 75)
        add_executable(lapis_supervisor_runtime_session_tests
            tests/supervisor_session_service_test.cpp)
        target_link_libraries(lapis_supervisor_runtime_session_tests PRIVATE
            lapis_project_options lapis_supervisor_runtime lapis_session_launch Qt6::Network)
        target_compile_definitions(lapis_supervisor_runtime_session_tests PRIVATE
            LAPIS_SUPERVISOR_SESSION_SERVICE="$<TARGET_FILE:lapis_session_service>")
        add_dependencies(lapis_supervisor_runtime_session_tests lapis_session_service)
        add_test(NAME supervisor-session-service
            COMMAND lapis_supervisor_runtime_session_tests)
        set_tests_properties(supervisor-session-service PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 75)
        add_executable(lapis_supervisor_daemon_tests tests/supervisor_daemon_test.cpp)
        target_include_directories(lapis_supervisor_daemon_tests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/include")
        target_link_libraries(lapis_supervisor_daemon_tests PRIVATE
            lapis_project_options lapis_supervisor_runtime lapis_session_launch
            lapis_local_protocol Qt6::Core Qt6::Network)
        target_compile_definitions(lapis_supervisor_daemon_tests PRIVATE
            LAPIS_SUPERVISOR_DAEMON="$<TARGET_FILE:lapis_supervisor_daemon>")
        target_compile_definitions(lapis_supervisor_daemon_tests PRIVATE
            LAPIS_SUPERVISOR_CLI="$<TARGET_FILE:lapis_supervisor>")
        add_dependencies(lapis_supervisor_daemon_tests lapis_supervisor_daemon lapis_supervisor)
        add_executable(lapis_supervisor_client_tests tests/supervisor_client_test.cpp)
        target_link_libraries(lapis_supervisor_client_tests PRIVATE
            lapis_project_options lapis_supervisor_runtime)
        add_test(NAME supervisor-client COMMAND lapis_supervisor_client_tests)
        set_tests_properties(supervisor-client PROPERTIES TIMEOUT 10)
        add_test(NAME supervisor-daemon COMMAND lapis_supervisor_daemon_tests)
        set_tests_properties(supervisor-daemon PROPERTIES TIMEOUT 90 SKIP_RETURN_CODE 75)
    endif()
endif()
