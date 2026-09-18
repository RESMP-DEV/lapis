# C++ verification and profiling

lapis uses clang-tidy as its primary C++ analyzer, Cppcheck for an independent
analysis, and runtime sanitizers for faults that static analysis cannot establish.
clangd supplies editor diagnostics from the same compilation database. Formatting
is automated so reviews can focus on behavior.

These tools are installed and exercised on the development Mac. The only current
C++ executable is a toolchain smoke program; passing it does not validate agent
sessions, a GUI, or frame smoothness. See the [verification receipt](../evidence/cpp-verification.json).

## Setup

On macOS, run `brew bundle --file Brewfile`. Xcode or its command-line tools must
provide an SDK. Python 3.11+ runs the verification scripts without extra packages.
`just` is an optional command shortcut, already present on this development machine.

The runner locates Homebrew LLVM without editing shell PATH or replacing Apple's
compiler. Set `LAPIS_LLVM_BIN` to another LLVM installation's `bin` directory to
select it explicitly. Keep clang++, clang-tidy, clang-format, and clangd from
the same LLVM installation. The current receipt records LLVM 23.1.1.

On Linux, install LLVM, Cppcheck, CMake, Ninja, a C++ standard library, and Python
through the platform's package manager; optionally install ccache. Linux execution
and Windows support have not been qualified. The current sanitizer presets reject
Windows rather than silently building without instrumentation.

## Commands

Run from the repository root:

| Command | What it checks |
| --- | --- |
| `just check` | Compiler warnings as errors, CTest, format, clang-tidy, and Cppcheck |
| `just asan` | CTest with AddressSanitizer and UndefinedBehaviorSanitizer |
| `just tsan` | CTest with ThreadSanitizer in a separate build |
| `just verify-tools` | Known-bad fixtures must produce specific failure diagnostics |
| `just format` | Apply C++ formatting |
| `just profile` | Optimized build with debug symbols and CTest |

Without `just`, use `python3 scripts/check_cpp.py dev`, replacing `dev` with
`asan`, `tsan`, `profile`, or `format` as appropriate. The detector check is
`python3 scripts/verify_cpp_tools.py`.

Builds use Ninja and ccache when available. Analysis runs in parallel, up to
eight workers by default; use `--jobs N` on `check_cpp.py` to adjust it. Each
invocation records diagnostics, tool versions, exit codes, and durations under
`build/reports/<mode>/`. Static analysis runs even when compilation is cached.

The development preset generates `build/dev/compile_commands.json`. Point your
editor's clangd extension at the same LLVM installation. `.clangd` supplies the
database location and limits interactive analysis to fast checks. The full batch
checks still run through `just check`.

## What is enforced

- Compiler warnings include conversions, shadowing, and virtual-function mistakes.
- clang-tidy enables the Clang static analyzer, bug-prone patterns, performance,
  portability, concurrency checks, and selected modern C++ practices. Reported
  project diagnostics fail the check. System-header diagnostics are excluded.
- A cognitive-complexity threshold of 25 flags functions that need simplification.
- Cppcheck uses the real compilation database, including defines and include paths.
- ASan catches exercised memory-access faults; UBSan catches exercised undefined
  behavior. TSan detects exercised races and is built separately from ASan.

Sanitizers inspect executed paths. They do not prove race freedom or memory
safety, and ASan is not a complete leak checker on macOS. Use Instruments
Allocations/Leaks for retention and leak investigation.

New CMake targets must link `lapis_project_options` so warning and sanitizer
settings apply. Add meaningful CTest cases for ownership, parsing, event ordering,
and input routing as those components arrive. Header-only code needs a compiled
consumer. Do not count the toolchain smoke as application test coverage.

## Measuring smoothness

The development Mac exposes Xcode Instruments templates for Time Profiler,
Animation Hitches, Allocations, Leaks, Game Performance, and Metal System Trace.
Their availability was checked with `xcrun xctrace list templates`. No graphical
workload has been traced yet.

Use the `profile` build for performance measurements; sanitizer overhead changes
timings. Once a desktop target exists:

1. Add timeline markers around input arrival, attention enqueue, focus ownership
   changes, terminal parsing, GPU submission, and frame presentation.
2. Use Time Profiler to find CPU work blocking interaction, Allocations for
   memory churn, and Metal System Trace for GPU work and synchronization stalls.
3. Add Qt Quick profiling once Qt is integrated to measure binding evaluations,
   scene-graph updates, and frame scheduling. Add clazy for Qt-specific C++
   diagnostics and qmllint for QML correctness when those sources exist.
4. Replay 32 sessions with controlled output rates. Record p50/p95/p99
   focus-to-first-frame latency and frame-time distributions, alongside CPU/GPU
   use and memory growth. Include an idle baseline and the actual display rate.

At 120 Hz, a frame interval is about 8.33 ms; at 60 Hz it is about 16.67 ms.
These are display budgets, not measured lapis results. Linters may expose
unnecessary copies or suspicious locking, but cannot establish that interaction
meets either budget.

## Dependency audit scope

This bootstrap adds development tools and standard-library-only C++. The shared
dependency audit ran; OSV found no supported project package sources. This is
not a vulnerability clearance for LLVM, Cppcheck, or future Qt dependencies.
Introduce appropriate lockfiles/SBOMs and audit them when runtime dependencies
are selected.
