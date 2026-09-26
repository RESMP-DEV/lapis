find_package(Qt6 6.11.2 EXACT REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2 Network)
find_package(Vulkan REQUIRED)
set(CMAKE_AUTOMOC ON)
if(APPLE)
    enable_language(OBJCXX)
endif()
# The downloadable app updates itself with Sparkle when it is given one; the
# feed is the appcast attached to the latest GitHub release.
set(LAPIS_SPARKLE_FRAMEWORK "" CACHE PATH "Sparkle.framework for the downloadable app's updates")
set(LAPIS_UPDATE_KEYS "")
if(LAPIS_PACKAGE AND LAPIS_SPARKLE_FRAMEWORK)
    file(READ macos/update-public-key.txt LAPIS_UPDATE_PUBLIC_KEY)
    string(STRIP "${LAPIS_UPDATE_PUBLIC_KEY}" LAPIS_UPDATE_PUBLIC_KEY)
    string(CONCAT LAPIS_UPDATE_KEYS
        "<key>SUFeedURL</key><string>https://github.com/RESMP-DEV/lapis/releases/latest/download/appcast.xml</string>"
        "<key>SUPublicEDKey</key><string>${LAPIS_UPDATE_PUBLIC_KEY}</string>"
        "<key>SUEnableAutomaticChecks</key><true/>")
endif()
add_library(lapis_desktop_view STATIC
    src/workspace.cpp src/workspace.hpp
    src/workspace_control.cpp src/workspace_control.hpp
    src/alerts.cpp src/alerts.hpp
    src/agent_search.cpp src/agent_search.hpp
    src/conversation_index.cpp src/conversation_index.hpp
    src/terminals.cpp src/terminals.hpp
    src/usage.cpp src/usage.hpp
    src/usage_limits.cpp src/usage_limits.hpp
    src/harness_models.cpp src/harness_models.hpp
    src/attention_view.cpp
    src/keymap.cpp src/keymap.hpp
    src/live_connection.cpp src/live_connection.hpp
    src/platform/updater_process.cpp src/platform/updater_process.hpp
    src/terminal_surface.cpp src/terminal_surface.hpp
    src/cell_shapes.cpp src/cell_shapes.hpp
    src/tile_layout.cpp src/tile_layout.hpp
    src/app_paths.cpp src/app_paths.hpp
    src/desktop_actions.cpp src/desktop_actions.hpp src/platform_desktop.hpp
    src/shell_environment.cpp src/shell_environment.hpp
    src/ui_preview.cpp src/ui_preview.hpp)
target_include_directories(lapis_desktop_view PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
if(APPLE)
    # Notifications, the login item, Command-` and (in the downloaded app) Sparkle.
    target_sources(lapis_desktop_view PRIVATE src/desktop_actions_mac.mm)
    target_link_libraries(lapis_desktop_view PUBLIC "-framework AppKit" "-framework Foundation"
        "-framework ServiceManagement" "-framework UserNotifications")
    if(LAPIS_PACKAGE AND LAPIS_SPARKLE_FRAMEWORK)
        get_filename_component(sparkle_directory "${LAPIS_SPARKLE_FRAMEWORK}" DIRECTORY)
        target_compile_definitions(lapis_desktop_view PRIVATE LAPIS_SPARKLE)
        target_compile_options(lapis_desktop_view PRIVATE "-F${sparkle_directory}")
        target_link_libraries(lapis_desktop_view PUBLIC "-F${sparkle_directory}" "-framework Sparkle")
    endif()
endif()
# The token counter another machine runs over ssh, compiled in as text.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS src/count_tokens.py)
file(READ src/count_tokens.py LAPIS_COUNT_TOKENS_SCRIPT)
configure_file(src/count_tokens.hpp.in "${CMAKE_CURRENT_BINARY_DIR}/generated/count_tokens.hpp" @ONLY)
target_include_directories(lapis_desktop_view PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(lapis_desktop_view PRIVATE lapis_project_options
    PUBLIC lapis_terminal lapis_local_protocol Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::Network)
if(LAPIS_PACKAGE)
    # The downloadable app finds everything inside its bundle and keeps its
    # data in ~/.lapis; no path on the build machine is compiled in.
    target_compile_definitions(lapis_desktop_view PRIVATE LAPIS_PACKAGED)
else()
    target_compile_definitions(lapis_desktop_view PRIVATE
        LAPIS_PROJECT_ROOT="${PROJECT_SOURCE_DIR}"
        LAPIS_SESSION_SERVICE_PATH="$<TARGET_FILE:lapis_session_service>"
        LAPIS_VULKAN_LIBRARY="${Vulkan_LIBRARY}")
endif()
qt_add_executable(lapis_desktop MACOSX_BUNDLE src/main.cpp src/ui_capture.cpp src/ui_capture.hpp)
if(APPLE)
    target_sources(lapis_desktop PRIVATE src/platform_preferences.mm)
    target_link_libraries(lapis_desktop PRIVATE "-framework AppKit")
endif()
target_link_libraries(lapis_desktop PRIVATE lapis_project_options lapis_desktop_view Qt6::QuickControls2)
qt_add_resources(lapis_desktop desktop_qml PREFIX "/" FILES qml/Main.qml qml/Settings.qml qml/AttentionDialog.qml qml/Commands.qml qml/AgentMark.qml qml/Search.qml qml/Resume.qml qml/Usage.qml)
set_target_properties(lapis_desktop PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/macos/Info.plist.in"
    MACOSX_BUNDLE_BUNDLE_NAME "lapis"
    MACOSX_BUNDLE_GUI_IDENTIFIER "dev.lapis.desktop"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    MACOSX_BUNDLE_ICON_FILE "lapis"
    MACOSX_BUNDLE_COPYRIGHT "Copyright (c) 2026 lapis contributors. MIT License.")
if(APPLE)
    target_sources(lapis_desktop PRIVATE macos/lapis.icns)
    set_source_files_properties(macos/lapis.icns PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
endif()
add_dependencies(lapis_desktop lapis_session_service)
if(LAPIS_PACKAGE)
    # lapis.app/Contents/MacOS/lapis, with the session service beside it.
    set_target_properties(lapis_desktop PROPERTIES OUTPUT_NAME "lapis")
    # The login item Settings turns on (SMAppService finds it by name).
    target_sources(lapis_desktop PRIVATE macos/dev.lapis.desktop.restore.plist)
    set_source_files_properties(macos/dev.lapis.desktop.restore.plist PROPERTIES
        MACOSX_PACKAGE_LOCATION Library/LaunchAgents)
    add_custom_command(TARGET lapis_desktop POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:lapis_session_service>"
            "$<TARGET_BUNDLE_CONTENT_DIR:lapis_desktop>/MacOS/lapis_session_service"
        VERBATIM)
endif()
if(BUILD_TESTING)
    add_executable(lapis_window_state_tests tests/window_state_test.cpp)
    target_link_libraries(lapis_window_state_tests PRIVATE lapis_project_options lapis_desktop_view Qt6::QuickControls2)
    target_compile_definitions(lapis_window_state_tests PRIVATE LAPIS_QML_SOURCE="${CMAKE_CURRENT_SOURCE_DIR}/qml/Main.qml")
    add_test(NAME window-state COMMAND lapis_window_state_tests)
    set_tests_properties(window-state PROPERTIES TIMEOUT 30 RESOURCE_LOCK "qt_gui" ENVIRONMENT "QT_MTL_NO_TRANSACTION=1;QT_VULKAN_LIB=${Vulkan_LIBRARY}")
    add_executable(lapis_conversation_index_tests tests/conversation_index_test.cpp)
    target_link_libraries(lapis_conversation_index_tests PRIVATE lapis_project_options lapis_desktop_view)
    add_test(NAME conversation-index COMMAND lapis_conversation_index_tests)
    set_tests_properties(conversation-index PROPERTIES TIMEOUT 60)
    add_executable(lapis_agent_search_tests tests/agent_search_test.cpp)
    target_link_libraries(lapis_agent_search_tests PRIVATE lapis_project_options lapis_desktop_view)
    add_test(NAME agent-search COMMAND lapis_agent_search_tests)
    set_tests_properties(agent-search PROPERTIES TIMEOUT 60)
    add_executable(lapis_app_environment_tests tests/app_environment_test.cpp)
    target_link_libraries(lapis_app_environment_tests PRIVATE lapis_project_options lapis_desktop_view)
    add_test(NAME app-environment COMMAND lapis_app_environment_tests)
    set_tests_properties(app-environment PROPERTIES TIMEOUT 20)
    add_executable(lapis_terminal_find_tests tests/terminal_find_test.cpp)
    target_link_libraries(lapis_terminal_find_tests PRIVATE lapis_project_options lapis_desktop_view)
    add_test(NAME terminal-find COMMAND lapis_terminal_find_tests)
    set_tests_properties(terminal-find PROPERTIES TIMEOUT 20)
    add_executable(lapis_tile_layout_tests tests/tile_layout_test.cpp)
    target_link_libraries(lapis_tile_layout_tests PRIVATE lapis_project_options lapis_desktop_view)
    add_test(NAME tile-layout COMMAND lapis_tile_layout_tests)
    set_tests_properties(tile-layout PROPERTIES TIMEOUT 20)
    add_executable(lapis_cell_shapes_tests tests/cell_shapes_test.cpp)
    target_link_libraries(lapis_cell_shapes_tests PRIVATE lapis_project_options lapis_desktop_view)
    add_test(NAME cell-shapes COMMAND lapis_cell_shapes_tests)
    set_tests_properties(cell-shapes PROPERTIES TIMEOUT 20)
    add_executable(lapis_harness_models_tests tests/harness_models_test.cpp)
    target_link_libraries(lapis_harness_models_tests PRIVATE lapis_project_options lapis_desktop_view)
    add_test(NAME harness-models COMMAND lapis_harness_models_tests)
    set_tests_properties(harness-models PROPERTIES TIMEOUT 60)
    add_executable(lapis_usage_tests tests/usage_test.cpp)
    target_link_libraries(lapis_usage_tests PRIVATE lapis_project_options lapis_desktop_view)
    add_test(NAME usage COMMAND lapis_usage_tests)
    set_tests_properties(usage PROPERTIES TIMEOUT 60)
    add_executable(lapis_workspace_tests tests/workspace_test.cpp)
    target_link_libraries(lapis_workspace_tests PRIVATE lapis_project_options lapis_desktop_view)
    # Closing an agent is exercised against the real session service, and the
    # workspace handover against the real windowless host.
    add_dependencies(lapis_workspace_tests lapis_session_service lapis_desktop)
    target_compile_definitions(lapis_workspace_tests PRIVATE
        LAPIS_DESKTOP_PATH="$<TARGET_FILE:lapis_desktop>")
    add_test(NAME workspace COMMAND lapis_workspace_tests)
    set_tests_properties(workspace PROPERTIES TIMEOUT 120)
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
    # Isolate focus ownership from the destructive QML engine-reload fixtures.
    add_test(NAME appearance-input COMMAND lapis_ui_preview_tests --shortcuts-only)
    set_tests_properties(ui-preview appearance-input PROPERTIES RESOURCE_LOCK "qt_gui" TIMEOUT 20 ENVIRONMENT "QT_MTL_NO_TRANSACTION=1;QT_VULKAN_LIB=${Vulkan_LIBRARY}")
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
    if(APPLE)
        foreach(test_target IN ITEMS lapis_ui_preview_tests lapis_terminal_input_tests)
            target_sources(${test_target} PRIVATE tests/platform/window_activation.mm)
            target_link_libraries(${test_target} PRIVATE "-framework AppKit")
        endforeach()
    endif()
endif()

# Opt-in real Codex workspace lifecycle; no GUI or model turn is submitted.
add_executable(lapis_workspace_live_probe tests/workspace_live_probe.cpp)
target_link_libraries(lapis_workspace_live_probe PRIVATE lapis_project_options lapis_desktop_view)
target_compile_definitions(lapis_workspace_live_probe PRIVATE
    LAPIS_SESSION_SERVICE_PATH="$<TARGET_FILE:lapis_session_service>")
add_dependencies(lapis_workspace_live_probe lapis_session_service)

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
        tests/platform/native_input_driver.mm tests/platform/window_activation.mm)
    target_link_libraries(lapis_native_input_probe PRIVATE lapis_project_options lapis_desktop_view
        "-framework AppKit" "-framework Carbon" "-framework CoreGraphics")
    target_compile_definitions(lapis_native_input_probe PRIVATE
        LAPIS_SESSION_SERVICE_PATH="$<TARGET_FILE:lapis_session_service>"
        LAPIS_VULKAN_LIBRARY="${Vulkan_LIBRARY}")
    add_dependencies(lapis_native_input_probe lapis_session_service)
endif()

# Uses a private service fixture supplied by check_service_attention.py --desktop.
add_executable(lapis_attention_ui_probe tests/attention_ui_probe.cpp)
if(APPLE)
    target_sources(lapis_attention_ui_probe PRIVATE tests/platform/window_activation.mm)
endif()
target_link_libraries(lapis_attention_ui_probe PRIVATE lapis_project_options lapis_desktop_view
    Qt6::QuickControls2)
target_compile_definitions(lapis_attention_ui_probe PRIVATE LAPIS_VULKAN_LIBRARY="${Vulkan_LIBRARY}")
qt_add_resources(lapis_attention_ui_probe attention_qml PREFIX "/" FILES
    qml/Main.qml qml/Settings.qml qml/AttentionDialog.qml qml/Commands.qml qml/AgentMark.qml qml/Search.qml qml/Resume.qml qml/Usage.qml)
