## What system/approach is used

The Lapis platform is built with **CMake 3.25+** (Ninja generator) for the C++/Qt core, **Gradle/Kotlin** for the Android app, and **Xcode project.yml** for iOS/Swift. A top-level **`justfile`** delegates every developer command to a single Python orchestrator at `scripts/lapis.py`, which in turn invokes CMake, Qt tools, and auxiliary packaging scripts. CI is minimal: only a GitHub Actions workflow that publishes the static `site/` download page to GitHub Pages.

## Key files and packages

- `CMakeLists.txt` — root CMake project (`lapis VERSION 0.5.0`), toggles subprojects via cache options, enables `CTest`.
- `CMakePresets.json` — named configure/build/test presets (`dev`, `asan`, `tsan`, `profile`, `desktop`, `r1-runtime`) using Ninja output under `build/<presetName>`.
- `cmake/ProjectOptions.cmake` — shared compile flags (`-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow …`), sanitizer wiring (`LAPIS_SANITIZER` = none/address/thread), compiler whitelist (AppleClang/Clang/GCC).
- `cmake/Ghostty.cmake` — fetches/pins the Ghostty VT engine used by terminal probes.
- `justfile` — user-facing commands (`bootstrap`, `check`, `asan`, `tsan`, `profile`, `format`, `desktop`, `run`, `ui-check`, `smoke`, `latency-budget`, …) all call `python3 scripts/lapis.py <cmd>`.
- `scripts/lapis.py` — dependency resolution, Ghostty bootstrap, CMake invocation wrapper, binary path discovery (`build/desktop/apps/desktop/lapis_desktop.app/Contents/MacOS/lapis_desktop` on macOS, plain executable on Linux).
- `Brewfile` — declares exact dev dependencies (llvm, cppcheck, cmake, ninja, ccache, qtdeclarative, vulkan-headers, vulkan-loader, molten-vk).
- `.github/workflows/site.yml` — publishes `site/` to GitHub Pages from `main`.
- Per-target build manifests: `apps/android/build.gradle.kts`, `apps/ios/project.yml`, `adapters/*/CMakeLists.txt`, `services/*/CMakeLists.txt`, `apps/desktop/CMakeLists.txt`, `apps/ultratab/CMakeLists.txt`.

## Architecture and conventions

### Multi-presets CMake configuration

All native builds go through the preset layer; direct `cmake` invocations are not the documented surface. Presets share a hidden `base` preset that sets:

- Generator: `Ninja`
- Binary dir: `${sourceDir}/build/${presetName}`
- `CMAKE_EXPORT_COMPILE_COMMANDS=ON`
- `BUILD_TESTING=ON`
- `LAPIS_SANITIZER=none`
- `LAPIS_WARNINGS_AS_ERRORS=ON`

Build types are chosen per preset: `Debug` (`dev`, `asan`, `tsan`), `RelWithDebInfo` (`profile`), and feature toggles (`LAPIS_BUILD_DESKTOP`, `LAPIS_BUILD_SESSION`, `LAPIS_BUILD_ULTRATAB`, `LAPIS_BUILD_SUPERVISOR_RUNTIME`, `LAPIS_BUILD_SUPERVISOR_STATE`). The root CMake enforces logical constraints:

- `LAPIS_BUILD_DESKTOP requires LAPIS_BUILD_SESSION` (enforced via `message(FATAL_ERROR ...)`)
- `LAPIS_BUILD_ULTRATAB requires LAPIS_BUILD_SESSION`
- Building desktop without session is fatal.

### Feature-gated subprojects

Each C++ component is its own CMake subproject added conditionally:

- `adapters/codex`, `adapters/claude` — adapter libraries linked into `lapis_session_service`.
- `services/session` — PTY/terminal service.
- `services/supervisor` — headless supervisor runtime (optionally pulled in when building the r1 runtime preset).
- `apps/desktop` — Qt desktop shell.
- `apps/ultratab` — overlay app (built alongside desktop unless `LAPIS_PACKAGE` is set; packaged separately by `scripts/package_ultratab.py`).

Tests live next to sources under `tests/` and are discovered via `add_subdirectory(...)/CMakeLists.txt` calling `add_test(...)`. A top-level smoke test (`tools/verification/smoke.cpp`) and the replay contract library (`tools/terminal_probe/replay.cpp`) are always built when `BUILD_TESTING` is on.

### Python orchestrator as single entry point

`scripts/lapis.py` centralizes environment setup: it resolves `LAPIS_GHOSTTY_PREFIX` (validated against `reports/receipt.json` and `tools/terminal_probe/ghostty/sources.json`), locates Qt/Vulkan/CTools, finds the desktop binary across platforms, and forwards arguments to CMake or launchers. This makes `just bootstrap`, `just check`, `just asan`, `just tsan`, `just profile`, `just format`, `just verify-tools`, `just ui-check`, `just cli-check`, `just history-check`, `just latency-budget`, etc. uniformly callable.

### Packaging and release

- Desktop distribution is produced by `scripts/package_macos.py` (called when `LAPIS_PACKAGE=ON`).
- Ultra Tab is packaged independently by `scripts/package_ultratab.py`.
- Release manifest generation lives in `scripts/release_manifest.py`.
- Version is defined once in the root `CMakeLists.txt` (`VERSION 0.5.0`).

### Platform-specific toolchains

- macOS/iOS: Xcode project via `project.yml`; signing entitlements under `apps/desktop/macos/*.entitlements` and `apps/ultratab/macos/*.entitlements`.
- Android: Gradle Kotlin DSL (`apps/android/build.gradle.kts`) with version catalog `gradle/libs.versions.toml`.
- Linux/macOS native: CMake + Ninja; Vulkan loader/MoltenVK required for UI preview.

### Conventions and constraints

- **Compiler requirement**: only AppleClang, Clang, or GCC are accepted; other compilers cause a fatal error in `cmake/ProjectOptions.cmake`.
- **C++ standard**: C++20, no extensions, enforced in root `CMakeLists.txt`.
- **Warnings as errors**: default-on via `LAPIS_WARNINGS_AS_ERRORS` (also enabled in `CMakePresets.json` base preset).
- **Sanitizers**: configured exclusively through the `LAPIS_SANITIZER` cache variable (`none` | `address` | `thread`); unknown values are rejected by `ProjectOptions.cmake`.
- **Supported hosts**: only `Darwin` and `Linux`; any other `CMAKE_SYSTEM_NAME` triggers a fatal error at configure time.
- **Dependency pinning**: Ghostty is fetched and validated against a pinned source list in `tools/terminal_probe/ghostty/sources.json`; the receipt file in `<prefix>/reports/receipt.json` must report `engine == "ghostty"` and `passed == True`.
- **Binary layout**: desktop output is a macOS bundle (`lapis_desktop.app/Contents/MacOS/lapis_desktop`) on macOS and a flat executable on Linux; paths are hard-coded in `scripts/lapis.py`'s `desktop_binary_path()`.
- **Test execution**: `CTest` is enabled globally; each preset's test preset uses `noTestsAction: error` so missing tests fail the run.
- **CI scope**: only the static site under `site/` is deployed via GitHub Actions; there is no automated native build/test workflow in this repository snapshot.