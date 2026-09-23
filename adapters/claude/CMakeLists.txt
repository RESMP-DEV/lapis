find_package(Qt6 6.11.2 EXACT REQUIRED COMPONENTS Core Network)
set(CMAKE_AUTOMOC ON)

add_library(lapis_claude_adapter STATIC
    src/claude_observer.cpp
    src/claude_observer.hpp
    src/hook_relay.cpp
    src/hook_relay.hpp)
target_link_libraries(lapis_claude_adapter
    PRIVATE lapis_project_options
    PUBLIC lapis_attention Qt6::Core Qt6::Network)
target_include_directories(lapis_claude_adapter PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src")

if(BUILD_TESTING)
    add_executable(lapis_claude_observer_tests tests/claude_observer_test.cpp)
    target_link_libraries(lapis_claude_observer_tests PRIVATE
        lapis_project_options lapis_claude_adapter)
    add_test(NAME claude-observer COMMAND lapis_claude_observer_tests)
    set_tests_properties(claude-observer PROPERTIES TIMEOUT 60)
endif()
