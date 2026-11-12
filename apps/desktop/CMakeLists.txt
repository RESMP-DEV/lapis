find_package(Qt6 6.11.2 EXACT REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2 Network)
find_package(Vulkan REQUIRED)
find_package(Threads REQUIRED)
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
add_library(lapis_desktop_core STATIC
    src/workspace.cpp src/workspace.hpp src/category_colors.hpp
    src/workspace_factory.cpp src/factory_state.cpp src/factory_state.hpp
    src/harness_catalog.cpp src/harness_catalog.hpp
    src/workspace_control.cpp src/workspace_control.hpp
    src/alerts.cpp src/alerts.hpp
    src/chime_sounds.cpp src/chime_sounds.hpp
    src/platform/chime_file.cpp src/platform/chime_file.hpp
    src/conversation_index.cpp src/conversation_index.hpp
    src/conversation_owner.cpp src/conversation_owner.hpp
    src/platform/process_files.cpp src/platform/process_files.hpp
    src/terminals.cpp src/terminals.hpp
    src/next_prompt.cpp src/next_prompt.hpp
    src/tab_ranker.cpp src/tab_ranker.hpp
    src/agent_state.cpp src/agent_state.hpp
    src/harness_models.cpp src/harness_models.hpp
    src/attention_view.cpp
    src/config.cpp src/config.hpp
    src/interaction_log.cpp src/interaction_log.hpp
    src/interaction_recorder.cpp src/interaction_recorder.hpp
    src/live_connection.cpp src/live_connection.hpp
    src/platform/updater_process.cpp src/platform/updater_process.hpp
    src/platform/published_task.hpp
    src/terminal_surface.cpp src/terminal_surface.hpp
    src/cell_shapes.cpp src/cell_shapes.hpp
    src/history_strip.cpp src/history_strip.hpp
    src/gui_state.cpp src/gui_state.hpp
    src/app_paths.cpp src/app_paths.hpp
    src/desktop_actions.cpp src/desktop_actions.hpp src/platform_desktop.hpp
    src/media_actions.cpp src/media_actions.hpp
    src/shell_environment.cpp src/shell_environment.hpp
    src/tool_status.cpp src/tool_status.hpp
    src/app_window.cpp src/app_window.hpp
    src/transcript.cpp src/transcript.hpp src/transcript_parts.hpp
    src/transcript_codex.cpp src/transcript_antigravity.cpp
    src/transcript_math.cpp src/transcript_math.hpp
    src/code_highlight.cpp src/code_highlight.hpp
    src/slash_commands.cpp src/slash_commands.hpp)
target_include_directories(lapis_desktop_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
if(APPLE)
    # Input idle time, the login item, Command-` and (in the downloaded app)
    # Sparkle.
    target_sources(lapis_desktop_core PRIVATE src/desktop_actions_mac.mm)
    target_link_libraries(lapis_desktop_core PUBLIC "-framework AppKit" "-framework Carbon"
        "-framework CoreGraphics" "-framework Foundation"
        "-framework ServiceManagement")
    if(LAPIS_PACKAGE AND LAPIS_SPARKLE_FRAMEWORK)
        get_filename_component(sparkle_directory "${LAPIS_SPARKLE_FRAMEWORK}" DIRECTORY)
        target_compile_definitions(lapis_desktop_core PRIVATE LAPIS_SPARKLE)
        target_compile_options(lapis_desktop_core PRIVATE "-F${sparkle_directory}")
        target_link_libraries(lapis_desktop_core PUBLIC "-F${sparkle_directory}" "-framework Sparkle")
    endif()
endif()
# The next-prompt helper, run here or sent to another machine, compiled in as text.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS src/next_prompt.py)
file(READ src/next_prompt.py LAPIS_NEXT_PROMPT_SCRIPT)
configure_file(src/next_prompt_script.hpp.in "${CMAKE_CURRENT_BINARY_DIR}/generated/next_prompt_script.hpp" @ONLY)
target_include_directories(lapis_desktop_core PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(lapis_desktop_core PRIVATE lapis_project_options lapis_session_platform
    lapis_claude_adapter lapis_terminal_hook_relay
    Threads::Threads
    PUBLIC lapis_terminal lapis_attention lapis_session_launch lapis_local_protocol
        Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::Network)
if(LAPIS_PACKAGE)
    # The downloadable app finds everything inside its bundle and keeps its
    # data in ~/.lapis; no path on the build machine is compiled in.
    target_compile_definitions(lapis_desktop_core PRIVATE LAPIS_PACKAGED)
else()
    target_compile_definitions(lapis_desktop_core PRIVATE
        LAPIS_PROJECT_ROOT="${PROJECT_SOURCE_DIR}"
        LAPIS_SESSION_SERVICE_PATH="$<TARGET_FILE:lapis_session_service>"
        LAPIS_VULKAN_LIBRARY="${Vulkan_LIBRARY}")
endif()
qt_add_executable(lapis_desktop MACOSX_BUNDLE src/main.cpp src/ui_capture.cpp src/ui_capture.hpp)
if(APPLE)
    target_sources(lapis_desktop PRIVATE src/platform_preferences.mm)
    target_link_libraries(lapis_desktop PRIVATE "-framework AppKit")
endif()
target_link_libraries(lapis_desktop PRIVATE lapis_project_options lapis_desktop_core Qt6::QuickControls2)
qt_add_resources(lapis_desktop desktop_qml PREFIX "/" FILES qml/Main.qml qml/Settings.qml qml/AttentionDialog.qml qml/Commands.qml qml/AgentMark.qml qml/Resume.qml qml/Tools.qml qml/Transcript.qml qml/MediaPlayback.qml qml/SettingsPreview.qml qml/FactoryBoard.qml)
# A development bundle has its own identity, so macOS never mistakes it for
# the installed app: `open` would otherwise bring the installed lapis forward,
# and the two would share notification and preference state.
if(LAPIS_PACKAGE)
    set(lapis_bundle_name "lapis")
    set(lapis_bundle_identifier "dev.lapis.desktop")
else()
    set(lapis_bundle_name "lapis dev")
    set(lapis_bundle_identifier "dev.lapis.desktop.dev")
endif()
set_target_properties(lapis_desktop PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/macos/Info.plist.in"
    MACOSX_BUNDLE_BUNDLE_NAME "${lapis_bundle_name}"
    MACOSX_BUNDLE_GUI_IDENTIFIER "${lapis_bundle_identifier}"
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
    target_link_libraries(lapis_window_state_tests PRIVATE lapis_project_options lapis_desktop_core Qt6::QuickControls2)
    target_compile_definitions(lapis_window_state_tests PRIVATE LAPIS_QML_SOURCE="${CMAKE_CURRENT_SOURCE_DIR}/qml/Main.qml")
    add_test(NAME window-state COMMAND lapis_window_state_tests)
    option(LAPIS_NATIVE_GUI_TESTS "Run GUI tests in native windows (takes focus)" OFF)
    if(LAPIS_NATIVE_GUI_TESTS)
        set(lapis_window_state_platform "QT_MTL_NO_TRANSACTION=1;QT_VULKAN_LIB=${Vulkan_LIBRARY}")
    else()
        # Offscreen with a virtual screen large enough for the saved-window
        # fixture: geometry is checked without showing a window.
        set(lapis_virtual_screen "${CMAKE_CURRENT_BINARY_DIR}/virtual_screen.json")
        file(WRITE "${lapis_virtual_screen}"
             "{\"screens\":[{\"name\":\"qualification\",\"x\":0,\"y\":0,\"width\":1920,\"height\":1080}]}\n")
        set(lapis_window_state_platform "QT_QPA_PLATFORM=offscreen:configfile=${lapis_virtual_screen}")
    endif()
    set_tests_properties(window-state PROPERTIES TIMEOUT 30 RESOURCE_LOCK "qt_gui" ENVIRONMENT "${lapis_window_state_platform}")
    add_executable(lapis_conversation_index_tests tests/conversation_index_test.cpp)
    target_link_libraries(lapis_conversation_index_tests PRIVATE lapis_project_options lapis_desktop_core)
    add_test(NAME conversation-index COMMAND lapis_conversation_index_tests)
    set_tests_properties(conversation-index PROPERTIES TIMEOUT 60)
    add_executable(lapis_app_environment_tests tests/app_environment_test.cpp)
    target_link_libraries(lapis_app_environment_tests PRIVATE lapis_project_options lapis_desktop_core)
    add_test(NAME app-environment COMMAND lapis_app_environment_tests)
    set_tests_properties(app-environment PROPERTIES TIMEOUT 20)
    add_executable(lapis_terminal_suggestion_tests tests/terminal_suggestion_test.cpp)
    target_link_libraries(lapis_terminal_suggestion_tests PRIVATE lapis_project_options lapis_desktop_core)
    add_test(NAME terminal-suggestion COMMAND lapis_terminal_suggestion_tests)
    set_tests_properties(terminal-suggestion PROPERTIES TIMEOUT 20)
    add_executable(lapis_gui_state_tests tests/gui_state_test.cpp)
    target_link_libraries(lapis_gui_state_tests PRIVATE lapis_project_options lapis_desktop_core)
    add_test(NAME gui-state COMMAND lapis_gui_state_tests)
    set_tests_properties(gui-state PROPERTIES TIMEOUT 30)
    add_executable(lapis_history_strip_tests tests/history_strip_test.cpp)
    target_link_libraries(lapis_history_strip_tests PRIVATE lapis_project_options lapis_desktop_core)
    add_test(NAME history-strip COMMAND lapis_history_strip_tests)
    set_tests_properties(history-strip PROPERTIES TIMEOUT 20)
    add_executable(lapis_media_actions_tests tests/media_actions_test.cpp)
    target_link_libraries(lapis_media_actions_tests PRIVATE lapis_project_options lapis_desktop_core)
    add_test(NAME media-actions COMMAND lapis_media_actions_tests)
    set_tests_properties(media-actions PROPERTIES TIMEOUT 20)
    add_executable(lapis_transcript_tests tests/transcript_test.cpp)
    target_link_libraries(lapis_transcript_tests PRIVATE lapis_project_options lapis_desktop_core)
    add_test(NAME transcript COMMAND lapis_transcript_tests)
    set_tests_properties(transcript PROPERTIES TIMEOUT 20)
    # The other CLIs' readers, against the synthetic conversations in tests/visual.
    add_executable(lapis_transcript_clis_tests tests/transcript_clis_test.cpp)
    target_link_libraries(lapis_transcript_clis_tests PRIVATE lapis_project_options
        lapis_desktop_core)
    target_compile_definitions(lapis_transcript_clis_tests PRIVATE
        LAPIS_VISUAL_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/tests/visual")
    add_test(NAME transcript-clis COMMAND lapis_transcript_clis_tests)
    set_tests_properties(transcript-clis PROPERTIES TIMEOUT 20)
    # Switching agents in the visual view: a cold thread's end first, threads
    # kept within a budget, kept ones following their files, the view's place.
    add_executable(lapis_transcript_switch_tests tests/transcript_switch_test.cpp)
    target_link_libraries(lapis_transcript_switch_tests PRIVATE lapis_project_options
        lapis_desktop_core)
    target_compile_definitions(lapis_transcript_switch_tests PRIVATE
        LAPIS_VISUAL_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/tests/visual"
        LAPIS_TRANSCRIPT_QML="${CMAKE_CURRENT_SOURCE_DIR}/qml/Transcript.qml")
    add_test(NAME transcript-switch COMMAND lapis_transcript_switch_tests)
    set_tests_properties(transcript-switch PROPERTIES TIMEOUT 60)
    # Offscreen contact sheet and scroll timing of the visual view (a review
    # tool); its --keys check of the bar's editing keys is a CTest entry.
    add_executable(lapis_transcript_capture tests/transcript_capture.cpp)
    target_link_libraries(lapis_transcript_capture PRIVATE lapis_project_options lapis_desktop_core)
    target_compile_definitions(lapis_transcript_capture PRIVATE
        LAPIS_TRANSCRIPT_QML="${CMAKE_CURRENT_SOURCE_DIR}/qml/Transcript.qml")
    add_test(NAME transcript-keys COMMAND lapis_transcript_capture --keys)
    set_tests_properties(transcript-keys PROPERTIES TIMEOUT 30)
    add_test(NAME transcript-live COMMAND lapis_transcript_capture --live-checks)
    add_test(NAME transcript-media COMMAND lapis_transcript_capture --media-checks)
    set_tests_properties(transcript-live transcript-media PROPERTIES TIMEOUT 30)
    add_executable(lapis_cell_shapes_tests tests/cell_shapes_test.cpp)
    target_link_libraries(lapis_cell_shapes_tests PRIVATE lapis_project_options lapis_desktop_core)
    add_test(NAME cell-shapes COMMAND lapis_cell_shapes_tests)
    set_tests_properties(cell-shapes PROPERTIES TIMEOUT 20)
    add_executable(lapis_harness_models_tests tests/harness_models_test.cpp)
    target_link_libraries(lapis_harness_models_tests PRIVATE lapis_project_options lapis_desktop_core)
    add_test(NAME harness-models COMMAND lapis_harness_models_tests)
    set_tests_properties(harness-models PROPERTIES TIMEOUT 60)
    add_executable(lapis_next_prompt_tests tests/next_prompt_test.cpp)
    target_link_libraries(lapis_next_prompt_tests PRIVATE lapis_project_options lapis_desktop_core)
    add_test(NAME next-prompt COMMAND lapis_next_prompt_tests)
    set_tests_properties(next-prompt PROPERTIES TIMEOUT 30)
    add_executable(lapis_tab_ranker_tests tests/tab_ranker_test.cpp)
    target_link_libraries(lapis_tab_ranker_tests PRIVATE lapis_project_options lapis_desktop_core)
    add_test(NAME tab-ranker COMMAND lapis_tab_ranker_tests)
    set_tests_properties(tab-ranker PROPERTIES TIMEOUT 30)
    add_executable(lapis_agent_state_tests tests/agent_state_test.cpp)
    target_link_libraries(lapis_agent_state_tests PRIVATE lapis_project_options lapis_desktop_core)
    add_test(NAME agent-state COMMAND lapis_agent_state_tests)
    set_tests_properties(agent-state PROPERTIES TIMEOUT 30)
    add_executable(lapis_workspace_tests tests/workspace_test.cpp)
    target_link_libraries(lapis_workspace_tests PRIVATE lapis_project_options lapis_desktop_core)
    # Closing an agent is exercised against the real session service, and the
    # workspace handover against the real windowless host.
    add_dependencies(lapis_workspace_tests lapis_session_service lapis_desktop)
    target_compile_definitions(lapis_workspace_tests PRIVATE
        LAPIS_DESKTOP_PATH="$<TARGET_FILE:lapis_desktop>")
    add_test(NAME workspace COMMAND lapis_workspace_tests)
    add_test(NAME workspace-remote-hooks COMMAND lapis_workspace_tests --case remote-hooks)
    set_tests_properties(workspace-remote-hooks PROPERTIES TIMEOUT 90)
    add_test(NAME workspace-adapter-status COMMAND lapis_workspace_tests --case adapter-status)
    set_tests_properties(workspace-adapter-status PROPERTIES TIMEOUT 20
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_test(NAME workspace-retired-plans COMMAND lapis_workspace_tests --case retired-plans)
    set_tests_properties(workspace-retired-plans PROPERTIES TIMEOUT 20)
    add_test(NAME workspace-updates-off COMMAND lapis_workspace_tests --case updates-off)
    set_tests_properties(workspace-updates-off PROPERTIES TIMEOUT 60)
    add_test(NAME workspace-omp-overlay COMMAND lapis_workspace_tests --case omp-overlay)
    set_tests_properties(workspace-omp-overlay PROPERTIES TIMEOUT 60)
    # A local Codex stand-in through the real session service; windowless.
    add_test(NAME workspace-local-codex COMMAND lapis_workspace_tests --case local-codex)
    set_tests_properties(workspace-local-codex PROPERTIES TIMEOUT 90
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    # The sidebar's walk and category colours; no process or window.
    add_test(NAME workspace-sidebar COMMAND lapis_workspace_tests --case sidebar)
    set_tests_properties(workspace-sidebar PROPERTIES TIMEOUT 20
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    # The visual view's files, with stand-in processes and a restore through
    # the real session service; windowless.
    add_test(NAME workspace-visual-files COMMAND lapis_workspace_tests --case visual-files)
    set_tests_properties(workspace-visual-files PROPERTIES TIMEOUT 60
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    # These launch the real desktop host or QML windows. On a Mac someone is
    # using they would show windows, so they run only on request.
    set_tests_properties(workspace workspace-remote-hooks workspace-retired-plans
                         workspace-updates-off PROPERTIES LABELS "screen")
    if(APPLE)
        add_executable(lapis_sound_decode_tests tests/sound_decode_test.mm)
        target_link_libraries(lapis_sound_decode_tests PRIVATE lapis_project_options lapis_desktop_core)
        add_test(NAME sound-decode COMMAND lapis_sound_decode_tests)
        set_tests_properties(sound-decode PROPERTIES TIMEOUT 20)
    endif()
    set_tests_properties(workspace PROPERTIES TIMEOUT 120)
    add_executable(lapis_live_connection_tests tests/live_connection_test.cpp)
    target_link_libraries(lapis_live_connection_tests PRIVATE lapis_project_options lapis_desktop_core)
    add_test(NAME live-connection COMMAND lapis_live_connection_tests)
    set_tests_properties(live-connection PROPERTIES TIMEOUT 30)
    add_executable(lapis_interaction_log_tests tests/interaction_log_test.cpp)
    target_link_libraries(lapis_interaction_log_tests PRIVATE lapis_project_options lapis_desktop_core
        Qt6::QuickControls2)
    target_compile_definitions(lapis_interaction_log_tests PRIVATE
        LAPIS_QML_SOURCE="${CMAKE_CURRENT_SOURCE_DIR}/qml/Main.qml")
    add_test(NAME interaction-log COMMAND lapis_interaction_log_tests)
    set_tests_properties(interaction-log PROPERTIES TIMEOUT 60
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_CONTROLS_STYLE=Basic")
    add_executable(lapis_config_tests tests/config_test.cpp)
    target_link_libraries(lapis_config_tests PRIVATE lapis_project_options lapis_desktop_core)
    add_test(NAME config COMMAND lapis_config_tests)
    set_tests_properties(config PROPERTIES TIMEOUT 20)

    # The factory view: the judge's file, stage rules, the orchestrator and
    # the workspace requests; and its window, always offscreen and software.
    add_executable(lapis_factory_tests tests/factory_test.cpp)
    target_link_libraries(lapis_factory_tests PRIVATE lapis_project_options lapis_desktop_core)
    add_test(NAME factory COMMAND lapis_factory_tests)
    set_tests_properties(factory PROPERTIES TIMEOUT 30
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_executable(lapis_factory_ui_tests tests/factory_ui_test.cpp)
    target_link_libraries(lapis_factory_ui_tests PRIVATE lapis_project_options lapis_desktop_core
        Qt6::QuickControls2)
    target_compile_definitions(lapis_factory_ui_tests PRIVATE
        LAPIS_QML_SOURCE="${CMAKE_CURRENT_SOURCE_DIR}/qml/Main.qml")
    add_test(NAME factory-ui COMMAND lapis_factory_ui_tests --background)
    set_tests_properties(factory-ui PROPERTIES TIMEOUT 60 RESOURCE_LOCK "qt_gui"
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_CONTROLS_STYLE=Basic")

    add_executable(lapis_tool_status_tests tests/tool_status_test.cpp)
    target_link_libraries(lapis_tool_status_tests PRIVATE lapis_project_options lapis_desktop_core)
    add_test(NAME tool-status COMMAND lapis_tool_status_tests)
    set_tests_properties(tool-status PROPERTIES TIMEOUT 30)

    add_executable(lapis_tools_ui_tests tests/tools_ui_test.cpp)
    target_link_libraries(lapis_tools_ui_tests PRIVATE lapis_project_options lapis_desktop_core
        Qt6::QuickControls2)
    target_compile_definitions(lapis_tools_ui_tests PRIVATE
        LAPIS_QML_SOURCE="${CMAKE_CURRENT_SOURCE_DIR}/qml/Main.qml")
    add_test(NAME tools-ui COMMAND lapis_tools_ui_tests)
    set_tests_properties(tools-ui PROPERTIES TIMEOUT 60 RESOURCE_LOCK "qt_gui" LABELS "screen" ENVIRONMENT
        "QT_MTL_NO_TRANSACTION=1;QT_VULKAN_LIB=${Vulkan_LIBRARY}")

    add_executable(lapis_ui_preview_tests tests/ui_preview_test.cpp tests/model_change_recorder.hpp)
    target_link_libraries(lapis_ui_preview_tests PRIVATE lapis_project_options lapis_desktop_core Qt6::QuickControls2)
    target_compile_definitions(lapis_ui_preview_tests PRIVATE LAPIS_QML_SOURCE="${CMAKE_CURRENT_SOURCE_DIR}/qml/Main.qml")
    # The desktop view embeds the service path; the offscreen fixture launches it.
    add_dependencies(lapis_ui_preview_tests lapis_session_service)
    # GUI tests run offscreen by default so they never show a window or take
    # focus on the machine someone is using; configure with
    # -DLAPIS_NATIVE_GUI_TESTS=ON for native windows, Vulkan and real focus.
    if(LAPIS_NATIVE_GUI_TESTS)
        set(lapis_gui_test_mode)
    else()
        set(lapis_gui_test_mode --background)
    endif()
    add_test(NAME ui-preview COMMAND lapis_ui_preview_tests ${lapis_gui_test_mode})
    # Isolate focus ownership from the destructive QML engine-reload fixtures.
    add_test(NAME appearance-input COMMAND lapis_ui_preview_tests ${lapis_gui_test_mode} --shortcuts-only)
    set_tests_properties(ui-preview PROPERTIES RESOURCE_LOCK "qt_gui" TIMEOUT 30 ENVIRONMENT "QT_MTL_NO_TRANSACTION=1;QT_VULKAN_LIB=${Vulkan_LIBRARY}")
    set_tests_properties(appearance-input PROPERTIES RESOURCE_LOCK "qt_gui" TIMEOUT 20 ENVIRONMENT "QT_MTL_NO_TRANSACTION=1;QT_VULKAN_LIB=${Vulkan_LIBRARY}")
    set_tests_properties(ui-preview appearance-input PROPERTIES LABELS "screen")
    add_executable(lapis_terminal_input_tests tests/terminal_input_test.cpp tests/link_receiver.hpp)
    target_link_libraries(lapis_terminal_input_tests PRIVATE lapis_project_options lapis_desktop_core)
    add_test(NAME terminal-input COMMAND lapis_terminal_input_tests ${lapis_gui_test_mode})
    set_tests_properties(terminal-input PROPERTIES TIMEOUT 45 RESOURCE_LOCK "qt_gui" ENVIRONMENT
        "QT_MTL_NO_TRANSACTION=1;QT_VULKAN_LIB=${Vulkan_LIBRARY}")
    add_executable(lapis_terminal_render_tests tests/terminal_render_test.cpp)
    target_link_libraries(lapis_terminal_render_tests PRIVATE lapis_project_options lapis_desktop_core)
    # Native Vulkan pixels need a shown window: only with LAPIS_NATIVE_GUI_TESTS.
    if(LAPIS_NATIVE_GUI_TESTS)
        add_test(NAME terminal-render COMMAND lapis_terminal_render_tests)
        set_tests_properties(terminal-render PROPERTIES TIMEOUT 20 RESOURCE_LOCK "qt_gui"
            ENVIRONMENT "QT_MTL_NO_TRANSACTION=1;QT_VULKAN_LIB=${Vulkan_LIBRARY}")
    endif()
    if(APPLE)
        add_executable(lapis_terminal_keys_mac_tests tests/terminal_keys_mac_test.mm)
        target_link_libraries(lapis_terminal_keys_mac_tests PRIVATE lapis_project_options lapis_desktop_core)
        add_test(NAME terminal-keys-mac COMMAND lapis_terminal_keys_mac_tests)
        # A shared NSApplication can surface in the Dock: on request only.
        set_tests_properties(terminal-keys-mac PROPERTIES TIMEOUT 20 LABELS "screen")
        foreach(test_target IN ITEMS lapis_ui_preview_tests lapis_terminal_input_tests)
            target_sources(${test_target} PRIVATE tests/platform/window_activation.mm)
            target_link_libraries(${test_target} PRIVATE "-framework AppKit")
        endforeach()
        # terminal-input --background is proven windowless (offscreen QPA, no
        # NSApplication activation). Tests labeled "screen" open windows, the
        # desktop host or an NSApplication, so a Mac runs them only on request.
        if(NOT LAPIS_NATIVE_GUI_TESTS)
            set_tests_properties(workspace workspace-remote-hooks workspace-retired-plans
                                 workspace-updates-off tools-ui ui-preview appearance-input
                                 terminal-keys-mac PROPERTIES DISABLED TRUE)
        endif()
    endif()
endif()

# Opt-in real Codex workspace lifecycle; no GUI or model turn is submitted.
add_executable(lapis_workspace_live_probe tests/workspace_live_probe.cpp)
target_link_libraries(lapis_workspace_live_probe PRIVATE lapis_project_options lapis_desktop_core)
target_compile_definitions(lapis_workspace_live_probe PRIVATE
    LAPIS_SESSION_SERVICE_PATH="$<TARGET_FILE:lapis_session_service>")
add_dependencies(lapis_workspace_live_probe lapis_session_service)

# Opt-in agent and category switching measurement (offscreen); not in CTest.
set(QT_NO_PRIVATE_MODULE_WARNING ON) # the benchmark is pinned to this exact Qt
find_package(Qt6 6.11.2 EXACT QUIET COMPONENTS GuiPrivate)
if(TARGET Qt6::GuiPrivate)
    add_executable(lapis_switch_benchmark tests/switch_benchmark.cpp)
    target_link_libraries(lapis_switch_benchmark PRIVATE lapis_project_options lapis_desktop_core
                                                         Qt6::QuickControls2 Qt6::GuiPrivate)
    target_compile_definitions(lapis_switch_benchmark PRIVATE
        LAPIS_QML_SOURCE="${CMAKE_CURRENT_SOURCE_DIR}/qml/Main.qml")
    add_dependencies(lapis_switch_benchmark lapis_session_service)
endif()

# Opt-in measurement, not a timing gate in CTest.
add_executable(lapis_terminal_latency_probe tests/terminal_latency_probe.cpp)
target_link_libraries(lapis_terminal_latency_probe PRIVATE lapis_project_options lapis_desktop_core)
target_compile_definitions(lapis_terminal_latency_probe PRIVATE
    LAPIS_SESSION_SERVICE_PATH="$<TARGET_FILE:lapis_session_service>"
    LAPIS_VULKAN_LIBRARY="${Vulkan_LIBRARY}")
add_dependencies(lapis_terminal_latency_probe lapis_session_service)

# OS-generated keyboard events through the real macOS IME and PTY service.
# Opt-in because posting events requires macOS Accessibility permission.
if(APPLE)
    add_executable(lapis_native_input_probe tests/native_input_probe.cpp
        tests/platform/native_input_driver.mm tests/platform/window_activation.mm)
    target_link_libraries(lapis_native_input_probe PRIVATE lapis_project_options lapis_desktop_core
        "-framework AppKit" "-framework Carbon" "-framework CoreGraphics")
    target_compile_definitions(lapis_native_input_probe PRIVATE
        LAPIS_SESSION_SERVICE_PATH="$<TARGET_FILE:lapis_session_service>"
        LAPIS_VULKAN_LIBRARY="${Vulkan_LIBRARY}")
    add_dependencies(lapis_native_input_probe lapis_session_service)
endif()
