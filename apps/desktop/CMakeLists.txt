find_package(Qt6 6.11.2 EXACT REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2 Network)
find_package(Vulkan REQUIRED)
set(CMAKE_AUTOMOC ON)
qt_add_executable(lapis_desktop MACOSX_BUNDLE
    src/main.cpp src/workspace.cpp src/workspace.hpp
    src/live_connection.cpp src/live_connection.hpp
    src/terminal_surface.cpp src/terminal_surface.hpp)
target_link_libraries(lapis_desktop PRIVATE lapis_project_options lapis_terminal lapis_local_protocol
    Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2 Qt6::Network)
target_compile_definitions(lapis_desktop PRIVATE
    LAPIS_PROJECT_ROOT="${PROJECT_SOURCE_DIR}"
    LAPIS_SESSION_SERVICE_PATH="$<TARGET_FILE:lapis_session_service>"
    LAPIS_VULKAN_LIBRARY="${Vulkan_LIBRARY}")
qt_add_resources(lapis_desktop desktop_qml PREFIX "/" FILES qml/Main.qml)
set_target_properties(lapis_desktop PROPERTIES
    MACOSX_BUNDLE_BUNDLE_NAME "lapis"
    MACOSX_BUNDLE_GUI_IDENTIFIER "dev.lapis.desktop")
add_dependencies(lapis_desktop lapis_session_service)
