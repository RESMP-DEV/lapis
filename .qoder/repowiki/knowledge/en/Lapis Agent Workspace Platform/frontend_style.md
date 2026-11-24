## Approach

Lapis ships four native frontends, each with its own styling system. There is no shared CSS/SCSS/Tailwind layer — styling is expressed in platform-native technologies:

| Platform | Framework | Styling mechanism |
|---|---|---|
| macOS/Linux desktop | Qt Quick (QML) + C++ backend | QML `palette` overrides + per-window color properties resolved from a runtime keymap theme object; colors are hex strings (`#070b10`, `#101820`, …) with fallback defaults when no keymap theme is present |
| Android | Jetpack Compose (Material3) | Custom `LapisTheme` wrapping `MaterialTheme` with a hand-crafted `darkColorScheme`; colors are defined as sRGB floats (`Color(0.36f, 0.52f, 1.0f)`) and mirrored value-for-value from the iOS app so both render identically |
| iOS (Lapis & Ultra Tab) | SwiftUI | `.preferredColorScheme(.dark)` pinned globally; color literals are declared as `sRGB` float triplets in Swift and referenced by name (`accent`, `background`, `panel`, …) |
| Ultra Tab overlay | Qt Quick (QML) | Minimal QML (`Overlay.qml`, `HarnessTile.qml`) inheriting the same palette conventions |

No CSS files live in the source tree; every match under `*.css` found by glob is vendored third-party output (Ghostty build web, Qt doc templates, Sparkle release notes). The repo's only site asset is `site/index.html`, which has no embedded stylesheet.

## Key Files

- `apps/desktop/qml/Main.qml` — root QML window defining `backgroundColor`, `surfaceColor`, `cardColor`, `focusedColor`, `borderColor`, `textColor`, `mutedTextColor`, `attentionColor`, `activityColor`, `faultColor`, plus density/typography derived properties (`uiFont`, `chromeFont`, `readoutFont`, `railWidth`, `tabHeight`). It sets `QtGui.QPalette` entries (`window`, `button`, `base`, `mid`, `highlight`, …) and resolves them from a `keymap.themes` entry matched by `keymap.themeName`, falling back to hard-coded dark hex values.
- `apps/desktop/qml/Commands.qml`, `Usage.qml`, `AttentionDialog.qml`, `Settings.qml`, `Tools.qml`, `Resume.qml`, `Search.qml`, `AgentMark.qml` — all QML screens that consume the `*Color` properties exposed on the root window or parent popups rather than declaring their own palettes.
- `apps/android/app/src/main/kotlin/dev/lapis/remote/ui/LapisTheme.kt` — single source of truth for Compose colors (`LapisColors.accent/background/panel/panelDeep/edge/quiet/live` + per-harness colors via `harness(id)`), wrapped in a `LapisTheme` composable that constructs a `darkColorScheme`. The file's Javadoc explicitly states it is "ported value-for-value" from the iOS app using the same sRGB floats.
- `apps/android/app/src/main/res/values/themes.xml` — minimal Android `Theme.Lapis` extending `android:Theme.Material.NoActionBar` with transparent status/navigation bars and black background; actual visual style lives entirely in Compose.
- `apps/ios/Lapis/LapisApp.swift` — pins `.preferredColorScheme(.dark)` at the app root; color definitions are co-located with UI views and share names with the Android `LapisColors` object.
- `apps/ultratab/qml/Overlay.qml`, `apps/ultratab/qml/HarnessTile.qml` — standalone overlay surfaces reusing the same dark-card aesthetic without a dedicated theme file.

## Architecture and Conventions

1. **Dark-first, opaque surfaces.** Every platform forces a dark palette. Android's AndroidManifest theme uses `@android:color/black` as `windowBackground`; iOS pins `.preferredColorScheme(.dark)`; QML falls back to `#070b10` background and `#101820` surface when no keymap theme is loaded.
2. **Accent is an edge, not a second surface.** The QML comment in `Main.qml` says selection is "a thin accent edge, not a second surface" — accent colors mark focus/selection borders (`focusedBorderColor`, `focusedColor`) while semantic states (attention, activity, fault) use distinct hues paired with shape/text cues.
3. **Cross-platform color sync by numeric precision.** Both Android (`LapisColors`) and iOS declare colors as raw sRGB floats (e.g. `Color(0.36f, 0.52f, 1.0f)`) rather than rounded hex, documented as ensuring identical rendering across platforms.
4. **Theming via runtime keymaps (desktop).** Desktop themes are not static assets; they are supplied at runtime through the `keymap.themes` array keyed by `themeName`. `Main.qml` looks up the matching theme and derives every color from it, keeping the QML shell theme-agnostic.
5. **Density-driven layout tokens.** QML exposes `densityMode` (`minimal`, `compact`, `comfortable`) that drives `uiFont`, `chromeFont`, `readoutFont`, `railWidth`, `tabHeight`, `stripHeight`, and `motionDuration` — responsive behavior is achieved by deriving sizes from font metrics and density, not by breakpoints.
6. **Terminal monospace separation.** A dedicated `monoFamily` property (resolved from the live terminal engine) isolates terminal and machine-readout typography from the UI face used for prose.
7. **Compose Material3 as the Android component library.** No custom design-system library is imported beyond `androidx.compose.material3.*`; all widgets are built from Material3 primitives (`Button`, `TextField`, `Scaffold`, dialogs).
8. **SwiftUI is the iOS framework.** All iOS screens import `SwiftUI` directly; there is no third-party SwiftUI component library.

## Conventions and Constraints

- QML screens consume color tokens exposed on the root window or popup objects (e.g. `usagePopup.surfaceColor`, `commandsPopup.borderColor`, `window.textColor`) instead of hard-coding hex values inside child components. This is observed across `Commands.qml`, `Usage.qml`, and other dialog QML files.
- When no keymap theme is available, QML always falls back to a fixed dark palette (`background=#070b10`, `surface=#101820`, `card=#15202b`, `hoveredCard=#1c2a38`, `focused=#1a3040`, `border=#243140`, `focusedBorder=#7ddec8`, `text=#e7eef4`, `mutedText=#8ea0b3`, `attention=#ff6b5a`, `attentionText=#1a0d0b`, `activity=#74a8ff`, `fault=#ee7a8a`, `plenty=#6fdc8c`, `scarce=#ff5f6d`).
- Android's `Theme.Lapis` intentionally leaves the entire visual theme to Compose; the XML theme only strips chrome and sets a black window background, as documented in the XML comment.
- The Android `LapisColors` object is kept synchronized with the iOS color definitions "value-for-value" using the same sRGB floats — this is a stated convention in the file's KDoc.
- Terminal content renders through the Ghostty engine (vendored under `third_party/ghostty`); its GTK/web CSS is not part of Lapis' styling system.
- No CSS/SCSS/Tailwind configuration exists in the repository source tree; any `*.css` matches are under `build/`, `third_party/`, or vendored sources.