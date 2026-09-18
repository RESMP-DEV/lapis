default:
    @just --list

# Compiler warnings, clang-format, clang-tidy, Cppcheck, and CTest.
check:
    python3 scripts/check_cpp.py dev

# Check instrumented execution for memory errors and undefined behavior.
asan:
    python3 scripts/check_cpp.py asan

# Check instrumented execution for data races (separate from ASan).
tsan:
    python3 scripts/check_cpp.py tsan

# An optimized build with debug symbols for a profiler.
profile:
    python3 scripts/check_cpp.py profile

# Apply the repository's C++ formatting rules.
format:
    python3 scripts/check_cpp.py format

# Verify that known defects actually fail the configured tools.
verify-tools:
    python3 scripts/verify_cpp_tools.py

# Build and check the runnable desktop preview.
desktop:
    python3 scripts/check_cpp.py desktop

# Open the built macOS application; its session service survives window closure.
run:
    open build/desktop/apps/desktop/lapis_desktop.app

# Isolated visual fixture: edit QML, then use Reload in the preview controls.
ui:
    build/desktop/apps/desktop/lapis_desktop.app/Contents/MacOS/lapis_desktop --ui-preview --qml "{{justfile_directory()}}/apps/desktop/qml/Main.qml"

# Launch the isolated fixture under the macOS native debugger.
ui-debug:
    xcrun lldb -- build/desktop/apps/desktop/lapis_desktop.app/Contents/MacOS/lapis_desktop --ui-preview --qml "{{justfile_directory()}}/apps/desktop/qml/Main.qml"

# Bounded isolated GUI and capture checks; never sends shell input.
ui-check:
    python3 scripts/check_ui_preview.py

# Dedicated service/desktop launch cases; no model turn or live-shell reuse.
cli-check:
    python3 scripts/check_cli_launch.py --desktop
