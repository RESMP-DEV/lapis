# Dependency Management

## Approach by Language / Build System

The repository has no single language; dependency management is split across four independent mechanisms:

| Layer | Manager | Pinning strategy | Lockfile / pin source |
|---|---|---|---|
| C++ / Qt desktop & services | CMake + system package manager (Homebrew) | Exact version via `find_package(Qt6 6.11.2 EXACT REQUIRED …)` | `Brewfile` pins the exact Qt runtime; CMake enforces the same version. |
| Android app | Gradle Version Catalog (`apps/android/gradle/libs.versions.toml`) | Central `[versions]` table with explicit semver for every library | `libs.versions.toml` is the single source of truth. |
| Native VT engine probe | External prebuilt artifact | Pinned manifest hash verified at build time | `tools/terminal_probe/ghostty/sources.json` + generated receipt in `build/…/reports/receipt.json`. |
| macOS dev toolchain | Homebrew | Exact versions in a top-level `Brewfile` | `Brewfile` declares llvm, cppcheck, cmake, ninja, ccache, qtdeclarative, vulkan-headers/loader, molten-vk. |

There is **no** `go.mod`, `package.json`, Cargo.toml, vcpkg, Conan, or `FetchContent()` usage in this repo.

## Key Files

- `Brewfile` — macOS developer-tool dependencies. Comment states: "CMake requires Qt 6.11.2 exactly".
- `cmake/Ghostty.cmake` — validates a prebuilt Ghostty VT static library against a pinned sources manifest before linking.
- `apps/android/gradle/libs.versions.toml` — version catalog for the Android companion app; explicitly constrains OkHttp, kotlinx.serialization/coroutines, Jetpack DataStore, Compose BOM 2026.09.00, AGP 9.4.1, Kotlin 2.3.21.
- `apps/desktop/CMakeLists.txt` — `find_package(Qt6 6.11.2 EXACT REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2 Network)` plus an optional `GuiPrivate` component found QUIETLY.
- `CMakeLists.txt` — top-level CMake that wires adapters/services/apps but does not fetch third-party code itself.
- `cmake/ProjectOptions.cmake` — shared compiler/linker options (no dependency resolution).
- `third_party/*/NOTICES.txt` — attribution-only files for vendored artifacts (Ghostty, MoltenVK, Qt, Sparkle); they contain license notices, not code.
- `justfile` — entry point invoking `scripts/lapis.py bootstrap`, which builds the pinned Ghostty VT artifact once per checkout.

## Architecture and Conventions

### Qt / C++: system packages + exact CMake pin
Qt is treated as an external system dependency discovered through CMake's `find_package`. The Brewfile installs `qtdeclarative` (which brings Qt 6.11.2 on the CI/dev machine), and `apps/desktop/CMakeLists.txt` demands the **exact** matching version. This avoids any CMake FetchContent / subdirectory vendoring of Qt.

### Ghostty VT engine: pinned prebuilt artifact with manifest verification
Ghostty is **not** vendored as source. Instead:
1. `scripts/probe_terminal.py --engine ghostty` downloads/builds a specific Ghostty revision and writes a `receipt.json` containing `engine`, `passed`, and `sources`.
2. `cmake/Ghostty.cmake` asserts that `LAPIS_GHOSTTY_PREFIX/include/ghostty/vt.h`, `libghostty-vt.a`, and the receipt exist.
3. It then compares the receipt's `sources` field against the committed `tools/terminal_probe/ghostty/sources.json` and fails if they differ.
4. Finally it exposes an imported `lapis_ghostty_vt` static library target.

This makes the Ghostty revision immutable to rebuilders — changing the pinned sources without updating the receipt is a hard error.

### Android: centralized version catalog, minimal allowed stack
`apps/android/gradle/libs.versions.toml` centralizes every version and alias. The file's header documents the approved dependency surface: "OkHttp, kotlinx.serialization, kotlinx.coroutines, Jetpack DataStore, Compose (+ material3-window-size-class). No DI framework, no Retrofit." All other modules reference libraries via `alias(libs.xxx)` from this catalog. AGP 9.4.1 uses its built-in Kotlin compiler, so only the compose and serialization compiler plugins are applied as standalone plugins.

### macOS dev tools: Brewfile as the single source of truth
`Brewfile` declares all non-Android dependencies needed to build the project locally (llvm, cppcheck, cmake, ninja, ccache, Qt, Vulkan headers/loader, MoltenVK). The comment links it back to `CONTRIBUTING.md` and the exact Qt 6.11.2 requirement.

### Vendored assets vs. vendored code
`third_party/` contains only NOTICES files (license attribution) for Ghostty, MoltenVK, Qt, and Sparkle. There is no vendored source tree — binaries and licenses are kept separate from the repository's build graph.

## Rules Observed

- **Qt version must be 6.11.2 exactly.** Enforced by `find_package(Qt6 6.11.2 EXACT REQUIRED …)` in `apps/desktop/CMakeLists.txt` and documented in the `Brewfile` comment referencing `CONTRIBUTING.md`.
- **Ghostty sources must match the committed manifest.** `cmake/Ghostty.cmake` calls `message(FATAL_ERROR …)` when the receipt's `sources` field differs from `tools/terminal_probe/ghostty/sources.json` or when the receipt reports `passed != ON`.
- **Only the declared Android dependency surface is allowed.** The version-catalog header enumerates the permitted libraries and explicitly excludes DI frameworks and Retrofit; new dependencies should go through `libs.versions.toml` rather than ad-hoc `implementation` declarations.
- **AGP 9.x is required for the Android build.** The catalog pins AGP 9.4.1 and the comments document why the standalone Kotlin Android plugin is dropped in favor of AGP 9's built-in Kotlin compiler.
- **No CMake FetchContent / Git submodules for third-party code.** Verified by grepping `*.cmake` for `fetch_content`, `FetchContent`, `ExternalProject`, `git clone`, or `submodule`; none matched. Third-party C++ dependencies come from the system (Qt, Threads) or prebuilt artifacts (Ghostty).
- **Attribution for third-party binaries lives under `third_party/*/NOTICES.txt`.** These files carry licensing information but no source code.