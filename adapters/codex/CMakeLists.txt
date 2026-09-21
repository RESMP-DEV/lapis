find_package(Qt6 6.11.2 EXACT REQUIRED COMPONENTS Core Network)
set(CMAKE_AUTOMOC ON)
add_library(lapis_codex_adapter STATIC
    src/unix_websocket.cpp src/unix_websocket.hpp
    src/observer.cpp src/observer.hpp)
target_link_libraries(lapis_codex_adapter PRIVATE lapis_project_options
    PUBLIC lapis_attention Qt6::Core Qt6::Network)
target_include_directories(lapis_codex_adapter PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
if(BUILD_TESTING)
    add_executable(lapis_codex_transport_tests tests/unix_websocket_test.cpp)
    target_link_libraries(lapis_codex_transport_tests PRIVATE lapis_project_options lapis_codex_adapter)
    add_test(NAME codex-transport COMMAND lapis_codex_transport_tests)
    set_tests_properties(codex-transport PROPERTIES TIMEOUT 20)
    add_executable(lapis_codex_observer_tests tests/observer_test.cpp)
    target_link_libraries(lapis_codex_observer_tests PRIVATE lapis_project_options lapis_codex_adapter)
    add_test(NAME codex-observer COMMAND lapis_codex_observer_tests)
    set_tests_properties(codex-observer PROPERTIES TIMEOUT 30)
endif()
