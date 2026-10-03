find_package(Qt6 6.11.2 EXACT REQUIRED COMPONENTS Core)

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
    add_library(lapis_supervisor_endpoint STATIC
        "${CMAKE_CURRENT_SOURCE_DIR}/../session/src/platform/posix/local_endpoint.cpp")
    target_include_directories(lapis_supervisor_endpoint
        PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../session/src")
    target_link_libraries(lapis_supervisor_endpoint
        PRIVATE lapis_project_options PUBLIC Qt6::Core)
    add_library(lapis_supervisor_runtime STATIC src/runtime.cpp src/control_server.cpp)
    target_include_directories(lapis_supervisor_runtime
        PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_link_libraries(lapis_supervisor_runtime
        PRIVATE lapis_project_options lapis_session_platform lapis_supervisor_endpoint
        PUBLIC lapis_supervisor_state Qt6::Core)
    add_executable(lapis_supervisor_child_fixture tests/supervisor_child_fixture.cpp)
    target_link_libraries(lapis_supervisor_child_fixture PRIVATE lapis_project_options)

    if(BUILD_TESTING)
        add_executable(lapis_supervisor_runtime_tests tests/supervisor_runtime_test.cpp)
        target_link_libraries(lapis_supervisor_runtime_tests PRIVATE
            lapis_project_options lapis_supervisor_runtime)
        target_compile_definitions(lapis_supervisor_runtime_tests PRIVATE
            LAPIS_SUPERVISOR_CHILD_FIXTURE="$<TARGET_FILE:lapis_supervisor_child_fixture>")
        add_test(NAME supervisor-runtime COMMAND lapis_supervisor_runtime_tests)
        set_tests_properties(supervisor-runtime PROPERTIES TIMEOUT 30)
    endif()
endif()
