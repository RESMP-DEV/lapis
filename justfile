default:
    @python3 scripts/lapis.py

# Common repository/Python quality checks, without a desktop or dependency build.
quality:
    python3 scripts/lapis.py quality

# The iPhone app's UI tests in a headless iOS Simulator, with a Mac-side client
# attached to the same agent (macOS with an iOS Simulator runtime).
ios-check:
    python3 scripts/check_ios_remote.py --codex --claude

# Report which local dependencies are ready.
doctor:
    python3 scripts/lapis.py doctor

# Build the pinned Ghostty VT dependency (once per checkout).
bootstrap:
    python3 scripts/lapis.py bootstrap

# Compiler warnings, clang-format, clang-tidy, Cppcheck, and CTest.
check:
    python3 scripts/lapis.py check

# Check instrumented execution for memory errors and undefined behavior.
asan:
    python3 scripts/lapis.py asan

# Check instrumented execution for data races (separate from ASan).
tsan:
    python3 scripts/lapis.py tsan

# An optimized build with debug symbols for a profiler.
profile:
    python3 scripts/lapis.py profile

# Apply the repository's C++ formatting rules.
format:
    python3 scripts/lapis.py format

# Verify that known defects actually fail the configured tools.
verify-tools:
    python3 scripts/lapis.py verify-tools

# Build and check the runnable desktop preview.
desktop:
    python3 scripts/lapis.py build

# Open the retained workspace; its session services survive window closure.
run:
    python3 scripts/lapis.py run

# Isolated visual fixture: edit QML, then use Reload in the preview controls.
ui:
    python3 scripts/lapis.py ui

# Launch the isolated fixture under the macOS native debugger.
ui-debug:
    python3 scripts/lapis.py ui-debug

# Bounded isolated GUI and capture checks; never sends shell input.
ui-check:
    python3 scripts/lapis.py ui-check

# Dedicated service/desktop launch cases; no model turn or live-shell reuse.
cli-check:
    python3 scripts/lapis.py cli-check

# Real service history, quotas and controlled macOS disk exhaustion.
history-check:
    python3 scripts/check_history.py --disk-full

# Correlated native-input/frame-submission measurement; run without competing GUI work.
latency:
    build/desktop/apps/desktop/lapis_terminal_latency_probe --native --samples 100 --output build/terminal-latency.json

# Automated AppKit keyboard, clipboard and real Japanese IME; run GUI checks serially.
native-input:
    build/desktop/apps/desktop/lapis_native_input_probe --output build/native-input.json

# Drive Qt key input through the PTY and capture the window.
smoke:
    python3 scripts/lapis.py smoke
