find_package(Qt6 6.11.2 EXACT REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2 Network)
find_package(Vulkan REQUIRED)
set(CMAKE_AUTOMOC ON)
add_library(lapis_desktop_view STATIC
    src/workspace.cpp src/workspace.hpp
    src/keymap.cpp src/keymap.hpp
    src/live_connection.cpp src/live_connection.hpp
    src/terminal_surface.cpp src/terminal_surface.hpp
    src/ui_preview.cpp src/ui_preview.hpp)
target_include_directories(lapis_desktop_view PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(lapis_desktop_view PRIVATE lapis_project_options
    PUBLIC lapis_terminal lapis_local_protocol Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::Network)
target_compile_definitions(lapis_desktop_view PRIVATE
    LAPIS_PROJECT_ROOT="${PROJECT_SOURCE_DIR}"
    LAPIS_SESSION_SERVICE_PATH="$<TARGET_FILE:lapis_session_service>")
qt_add_executable(lapis_desktop MACOSX_BUNDLE src/main.cpp src/ui_capture.cpp src/ui_capture.hpp)
if(APPLE)
    enable_language(OBJCXX)
    target_sources(lapis_desktop PRIVATE src/platform_preferences.mm)
endif()
target_link_libraries(lapis_desktop PRIVATE lapis_project_options lapis_desktop_view Qt6::QuickControls2)
target_compile_definitions(lapis_desktop PRIVATE LAPIS_VULKAN_LIBRARY="${Vulkan_LIBRARY}"
    LAPIS_PROJECT_ROOT="${PROJECT_SOURCE_DIR}")
qt_add_resources(lapis_desktop desktop_qml PREFIX "/" FILES qml/Main.qml qml/Settings.qml)
set_target_properties(lapis_desktop PROPERTIES
    MACOSX_BUNDLE_BUNDLE_NAME "lapis"
    MACOSX_BUNDLE_GUI_IDENTIFIER "dev.lapis.desktop")
add_dependencies(lapis_desktop lapis_session_service)
if(BUILD_TESTING)
    add_executable(lapis_live_connection_tests tests/live_connection_test.cpp)
    target_link_libraries(lapis_live_connection_tests PRIVATE lapis_project_options lapis_desktop_view)
    add_test(NAME live-connection COMMAND lapis_live_connection_tests)
    set_tests_properties(live-connection PROPERTIES TIMEOUT 30)
    add_executable(lapis_keymap_tests tests/keymap_test.cpp)
    target_link_libraries(lapis_keymap_tests PRIVATE lapis_project_options lapis_desktop_view)
    add_test(NAME keymap COMMAND lapis_keymap_tests)
    set_tests_properties(keymap PROPERTIES TIMEOUT 20)

    add_executable(lapis_ui_preview_tests tests/ui_preview_test.cpp)
    target_link_libraries(lapis_ui_preview_tests PRIVATE lapis_project_options lapis_desktop_view Qt6::QuickControls2)
    target_compile_definitions(lapis_ui_preview_tests PRIVATE LAPIS_QML_SOURCE="${CMAKE_CURRENT_SOURCE_DIR}/qml/Main.qml")
    add_test(NAME ui-preview COMMAND lapis_ui_preview_tests)
    set_tests_properties(ui-preview PROPERTIES RESOURCE_LOCK "qt_gui" TIMEOUT 20 ENVIRONMENT "QT_MTL_NO_TRANSACTION=1;QT_VULKAN_LIB=${Vulkan_LIBRARY}")
    add_executable(lapis_terminal_input_tests tests/terminal_input_test.cpp)
    target_link_libraries(lapis_terminal_input_tests PRIVATE lapis_project_options lapis_desktop_view)
    add_test(NAME terminal-input COMMAND lapis_terminal_input_tests)
    set_tests_properties(terminal-input PROPERTIES TIMEOUT 30 RESOURCE_LOCK "qt_gui" ENVIRONMENT
        "QT_MTL_NO_TRANSACTION=1;QT_VULKAN_LIB=${Vulkan_LIBRARY}")
    add_executable(lapis_terminal_render_tests tests/terminal_render_test.cpp)
    target_link_libraries(lapis_terminal_render_tests PRIVATE lapis_project_options lapis_desktop_view)
    add_test(NAME terminal-render COMMAND lapis_terminal_render_tests)
    set_tests_properties(terminal-render PROPERTIES TIMEOUT 20 RESOURCE_LOCK "qt_gui" ENVIRONMENT
        "QT_MTL_NO_TRANSACTION=1;QT_VULKAN_LIB=${Vulkan_LIBRARY}")
endif()

# Opt-in measurement, not a timing gate in CTest.
add_executable(lapis_terminal_latency_probe tests/terminal_latency_probe.cpp)
target_link_libraries(lapis_terminal_latency_probe PRIVATE lapis_project_options lapis_desktop_view)
target_compile_definitions(lapis_terminal_latency_probe PRIVATE
    LAPIS_SESSION_SERVICE_PATH="$<TARGET_FILE:lapis_session_service>"
    LAPIS_VULKAN_LIBRARY="${Vulkan_LIBRARY}")
add_dependencies(lapis_terminal_latency_probe lapis_session_service)

# OS-generated keyboard events through the real macOS IME and PTY service.
# Opt-in because posting events requires macOS Accessibility permission.
if(APPLE)
    add_executable(lapis_native_input_probe tests/native_input_probe.cpp
        tests/platform/native_input_driver.mm)
    target_link_libraries(lapis_native_input_probe PRIVATE lapis_project_options lapis_desktop_view
        "-framework AppKit" "-framework Carbon" "-framework CoreGraphics")
    target_compile_definitions(lapis_native_input_probe PRIVATE
        LAPIS_SESSION_SERVICE_PATH="$<TARGET_FILE:lapis_session_service>"
        LAPIS_VULKAN_LIBRARY="${Vulkan_LIBRARY}")
    add_dependencies(lapis_native_input_probe lapis_session_service)
endif()
