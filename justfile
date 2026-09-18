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
