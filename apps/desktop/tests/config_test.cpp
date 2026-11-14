// Appearance configuration behavior: theme and density selection,
// validation of unknown names, and persistence back to the config file.
//
// The config reads a real file rather than a stub, so these cases exercise the
// same path the settings dialog uses, including the write-back that makes a
// choice survive a restart.
#include "config.hpp"

#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QKeySequence>
#include <QList>
#include <QPair>
#include <QSaveFile>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QThread>
#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <iostream>
#include <stdexcept>

namespace {
using lapis::desktop::CardDensity;
using lapis::desktop::Config;
using lapis::desktop::default_settings_shortcuts;
using lapis::desktop::theme_table;

void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}

[[nodiscard]] QString write_config(const QDir& directory, const QByteArray& contents) {
    const QString path = directory.filePath(QStringLiteral("lapis.json"));
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        throw std::runtime_error("could not create the test config");
    file.write(contents);
    file.close();
    return path;
}

[[nodiscard]] QJsonObject read_config(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        throw std::runtime_error("could not read the test config");
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    if (!document.isObject())
        throw std::runtime_error("test config is not a JSON object");
    return document.object();
}

// The theme table is what the dialog renders and what C++ resolves names
// against, so every entry must carry a complete, distinct palette.
void themes_are_complete() {
    const auto& themes = theme_table();
    require(themes.size() >= 4, "expected a set of built-in themes");
    for (std::size_t i = 0; i < themes.size(); ++i) {
        const auto& theme = themes[i];
        require(theme.name != nullptr && *theme.name != '\0', "theme needs a name");
        require(theme.label != nullptr && *theme.label != '\0', "theme needs a label");
        const char* fields[] = {theme.background, theme.surface,        theme.card,
                                theme.border,     theme.text,           theme.muted_text,
                                theme.attention,  theme.focused_border, theme.activity,
                                theme.fault,      theme.plenty};
        for (const char* value : fields)
            require(value != nullptr && *value == '#', "theme colour must be a hex literal");
        // Keyboard focus, activity, pending requests and faults each keep one
        // meaning, so no two of them may share a colour within a theme.
        const std::array semantic{
            QString::fromLatin1(theme.focused_border), QString::fromLatin1(theme.activity),
            QString::fromLatin1(theme.attention), QString::fromLatin1(theme.fault)};
        for (std::size_t a = 0; a < semantic.size(); ++a)
            for (std::size_t b = a + 1; b < semantic.size(); ++b)
                require(semantic.at(a).compare(semantic.at(b), Qt::CaseInsensitive) != 0,
                        "focus, activity, attention and fault colours must differ");
        // Names are the config keys, so a duplicate would make one unreachable.
        for (std::size_t j = i + 1; j < themes.size(); ++j)
            require(QString::fromLatin1(themes[i].name) != QString::fromLatin1(themes[j].name),
                    "theme names must be unique");
    }
}

// OLED black leaves every resting surface unlit.
void oled_theme_is_black() {
    const auto& oled = lapis::desktop::theme_for(QStringLiteral("oled"));
    require(QString::fromLatin1(oled.name) == QStringLiteral("oled"), "oled theme exists");
    for (const char* value : {oled.background, oled.surface, oled.card})
        require(QString::fromLatin1(value) == QStringLiteral("#000000"),
                "OLED resting surfaces are pure black");
}

// A selection is applied in memory and written to disk immediately, so the
// window and the file never disagree after the dialog closes.
void selection_persists() {
    QTemporaryDir directory;
    require(directory.isValid(), "temporary directory");
    const QDir dir(directory.path());
    const QString path = write_config(
        dir,
        R"({"version": 1, "layout": "focus", "keybindings": {"quit": ["Ctrl+Q"], "cycleLayout": ["Ctrl+L"]}})");

    Config config;
    require(config.load(), "default config should load");
    config.setSourcePathForTesting(path);
    require(config.load(), "explicit config should load");
    // "layout" is left from the retired session layouts; it loads silently.
    require(config.diagnostic().isEmpty(), "a legacy layout key should load without complaint");

    require(config.setTheme(QStringLiteral("graphite")), "graphite theme should apply");
    require(config.themeName() == QStringLiteral("graphite"), "theme should be remembered");
    require(config.setDensity(QStringLiteral("compact")), "compact density should apply");
    require(config.densityName() == QStringLiteral("compact"), "density should be remembered");

    const QJsonObject written = read_config(path);
    require(written.value(QStringLiteral("theme")).toString() == QStringLiteral("graphite"),
            "theme should be written to the file");
    require(written.value(QStringLiteral("density")).toString() == QStringLiteral("compact"),
            "density should be written to the file");
    // Rewriting appearance must not discard the rest of the user's config.
    require(written.value(QStringLiteral("version")).toInt() == 1,
            "unrelated config keys should survive a write");
    require(written.value(QStringLiteral("keybindings")).isObject(),
            "keybindings should survive a write");

    // The file is hand-edited, so a written sequence list must stay on one line.
    QFile raw(path);
    require(raw.open(QIODevice::ReadOnly), "rewritten config should be readable");
    const QString text = QString::fromUtf8(raw.readAll());
    raw.close();
    require(text.contains(QStringLiteral(R"("Ctrl+Q")")), "key sequences should stay readable");
    require(!text.contains(QStringLiteral("[\n")),
            "arrays should not be exploded one value per line");

    // A fresh instance reads the same values back, which is what a restart does.
    Config reloaded;
    reloaded.setSourcePathForTesting(path);
    require(reloaded.load(), "written config should reload");
    require(reloaded.themeName() == QStringLiteral("graphite"), "theme should survive a reload");
    require(reloaded.densityName() == QStringLiteral("compact"), "density should survive a reload");
}

void symbolic_link_config_updates_target() {
    QTemporaryDir directory;
    require(directory.isValid(), "temporary directory");
    const QDir dir(directory.path());
    const QString target = write_config(dir, R"({"layout":"focus","retained":true})");
    const QString link = dir.filePath(QStringLiteral("linked.json"));
    const QString chain = dir.filePath(QStringLiteral("chain.json"));
    require(QFile::link(QStringLiteral("lapis.json"), link), "relative config symlink");
    require(QFile::link(link, chain), "absolute config symlink chain");
    const QString canonical_target = QFileInfo(target).canonicalFilePath();
    require(QFileInfo(chain).canonicalFilePath() == canonical_target,
            "fixture symlink does not refer to the temporary config");
    Config config;
    config.setSourcePathForTesting(chain);
    require(config.load(), "linked config should load");
    require(config.setTheme(QStringLiteral("graphite")), "linked config should save");
    require(QFileInfo(link).isSymbolicLink() && QFileInfo(chain).isSymbolicLink(),
            "appearance save replaced a config symlink");
    require(QFileInfo(chain).canonicalFilePath() == canonical_target, "config link target changed");
    const auto saved = read_config(target);
    require(saved.value(QStringLiteral("theme")).toString() == QStringLiteral("graphite") &&
                saved.value(QStringLiteral("retained")).toBool(),
            "linked save did not preserve and update the actual target");
    require(config.load() && config.themeName() == QStringLiteral("graphite"),
            "linked config did not reload");
}

// Appearance changes must round-trip every unrelated JSON value exactly.
void appearance_preserves_json_and_bad_files() {
    QTemporaryDir directory;
    require(directory.isValid(), "temporary directory");
    const QDir dir(directory.path());
    const QByteArray original =
        R"({"keybindings":{"openSettings":["Ctrl+,"],"nextWindow":["Ctrl+Shift+]"],"custom":["a, b","quote\"[]", "space  space"]},"categories":[{"title":"[one],  two"}],"layout":"focus"})";
    const QString path = write_config(dir, original);
    const QJsonObject before = read_config(path);
    Config config;
    config.setSourcePathForTesting(path);
    require(config.load(), "load punctuation fixture");
    require(config.setTheme(QStringLiteral("graphite")), "persist punctuation fixture");
    const QJsonObject after = read_config(path);
    require(after.value(QStringLiteral("keybindings")) ==
                before.value(QStringLiteral("keybindings")),
            "appearance save changed a shortcut");
    require(after.value(QStringLiteral("categories")) == before.value(QStringLiteral("categories")),
            "appearance save changed category text");
    const QByteArray malformed = "{unfinished user edit";
    static_cast<void>(write_config(dir, malformed));
    require(!config.setDensity(QStringLiteral("compact")), "refuse to overwrite invalid JSON");
    QFile raw(path);
    require(raw.open(QIODevice::ReadOnly), "read invalid config");
    require(raw.readAll() == malformed, "invalid config must be preserved exactly");
    require(!config.diagnostic().isEmpty(), "failed save is visible");
}

// Pretty whitespace must not make a valid deeply nested config grow beyond the
// read cap. Qt itself has a finite parser limit; this depth is within it.
void deeply_nested_values_survive_persistence() {
    QTemporaryDir directory;
    require(directory.isValid(), "temporary directory");
    QJsonValue nested(QStringLiteral("leaf"));
    for (int depth = 0; depth < 768; ++depth) {
        if (depth % 2 == 0)
            nested = QJsonObject{{QStringLiteral("child"), nested}};
        else
            nested = QJsonArray{nested};
    }
    QJsonObject root{{QStringLiteral("nested"), nested}};
    const QDir dir(directory.path());
    const QString path =
        write_config(dir, QJsonDocument(root).toJson(QJsonDocument::JsonFormat::Compact) + '\n');

    Config config;
    config.setSourcePathForTesting(path);
    require(config.load(), "768-level JSON is accepted by Qt and loaded");
    require(config.setTheme(QStringLiteral("graphite")), "deep config should save");

    const QJsonObject written = read_config(path);
    require(written.value(QStringLiteral("nested")) == nested,
            "deep nested values should be preserved");
    Config reloaded;
    reloaded.setSourcePathForTesting(path);
    require(reloaded.load(), "saved deep config should reload below the byte cap");
    require(reloaded.themeName() == QStringLiteral("graphite"), "deep config reload state");
}

// Empty structures carry no human-facing content, so pretty printing should not
// spend four lines on them.
void empty_objects_are_compact() {
    QTemporaryDir directory;
    require(directory.isValid(), "temporary directory");
    const QDir dir(directory.path());
    const QString path =
        write_config(dir, QByteArrayLiteral(R"({"empty":{},"nested":{"empty":{}},"version":1})"));

    Config config;
    config.setSourcePathForTesting(path);
    require(config.load(), "empty-object fixture should load");
    require(config.setDensity(QStringLiteral("minimal")), "empty-object fixture should save");

    QFile raw(path);
    require(raw.open(QIODevice::ReadOnly), "compact config should be readable");
    const QString text = QString::fromUtf8(raw.readAll());
    raw.close();
    require(text.contains(QStringLiteral("\"empty\": {}")),
            "empty objects should be serialized compactly");
    require(text.contains(QStringLiteral("\"nested\": {")) &&
                !text.contains(QStringLiteral("\"nested\": {}}")),
            "non-empty objects should remain pretty-printed");
}

void expanded_config_remains_reloadable() {
    QTemporaryDir directory;
    require(directory.isValid(), "temporary directory");
    QJsonArray entries;
    for (int index = 0; index < 30000; ++index)
        entries.append(QJsonObject{{QStringLiteral("v"), index}});
    const auto contents = QJsonDocument(QJsonObject{{QStringLiteral("entries"), entries}})
                              .toJson(QJsonDocument::Compact);
    const auto path = write_config(QDir(directory.path()), contents);
    Config config;
    config.setSourcePathForTesting(path);
    require(config.load(), "compact array fixture loads");
    require(config.setTheme(QStringLiteral("amber")), "large config saves within read cap");
    require(config.reload(), "saved large config reloads");
    require(read_config(path).value(QStringLiteral("entries")).toArray() == entries,
            "compaction preserves every array entry");
}

// An absent config is a first launch, but a partially written non-empty file is
// treated as user data and never overwritten.
void zero_byte_file_initializes() {
    QTemporaryDir directory;
    require(directory.isValid(), "temporary directory");
    const QDir dir(directory.path());
    const QString path = write_config(dir, QByteArray{});

    Config config;
    config.setSourcePathForTesting(path);
    require(!config.load(), "zero-byte file has no JSON object yet");
    require(config.setTheme(QStringLiteral("daylight")), "zero-byte file should initialize");
    const QJsonObject written = read_config(path);
    require(written.value(QStringLiteral("theme")).toString() == QStringLiteral("daylight"),
            "initialized config should contain the first selection");
    require(written.value(QStringLiteral("version")).toInt() == 1,
            "initialized config should contain the schema version");
}

// Directories, FIFOs, and devices must not be opened as configuration. The
// directory case also verifies that persist and load share the same guard.
void nonregular_config_paths_are_rejected() {
    QTemporaryDir directory;
    require(directory.isValid(), "temporary directory");
    QDir dir(directory.path());
    require(dir.mkdir(QStringLiteral("config")), "create directory config path");
    const QString path = dir.filePath(QStringLiteral("config"));

    Config config;
    config.setSourcePathForTesting(path);
    require(!config.load(), "load must reject a nonregular path");
    require(!config.save(), "persist must reject a nonregular path");
}

// The dialog must expose both platform spellings without changing custom
// sequences loaded from the user's config.
void default_settings_chords_are_shared() {
#ifdef Q_OS_MACOS
    const QStringList expected{QStringLiteral("Meta+,")};
#else
    const QStringList expected{QStringLiteral("Ctrl+Shift+,")};
#endif
    require(default_settings_shortcuts() == expected,
            "settings defaults must preserve terminal Control chords");
    Config config;
    require(config.actionSequences(QStringLiteral("openSettings")) == expected,
            "Config defaults must use the exported settings chords");

    QTemporaryDir directory;
    require(directory.isValid(), "temporary directory for custom binding");
    const QDir dir(directory.path());
    const QString path =
        write_config(dir, QByteArrayLiteral(R"({"keybindings":{"openSettings":[" Alt+F4 "]}})"));
    config.setSourcePathForTesting(path);
    require(config.load(), "custom settings fixture should load");
    const QStringList custom = config.actionSequences(QStringLiteral("openSettings"));
    require(custom == QStringList{QStringLiteral("Alt+F4")},
            "custom bindings must replace defaults exactly, with only surrounding space trimmed");
    using lapis::desktop::shifted_punctuation;
    const auto control_shift = Qt::ControlModifier | Qt::ShiftModifier;
    require(shifted_punctuation(QKeyCombination(control_shift, Qt::Key_Comma)) ==
                QKeyCombination(control_shift, Qt::Key_Less),
            "X11's Ctrl+Shift+< is the Ctrl+Shift+, chord");
    require(!shifted_punctuation(QKeyCombination(Qt::ControlModifier, Qt::Key_Comma)) &&
                !shifted_punctuation(QKeyCombination(control_shift, Qt::Key_P)),
            "only Shift with punctuation has a shifted form");
}

// Actions lapis no longer has. A lapis.json that still binds them loads as
// before; the bindings reach nothing.
constexpr std::array kRetiredActions{
    "nextAttention", "latestAttention", "searchAgents", "find",     "splitRight", "splitDown",
    "tileLeft",      "tileRight",       "tileUp",       "tileDown", "zoomTile"};

void retired_actions_load_silently() {
    QTemporaryDir directory;
    require(directory.isValid(), "temporary directory for retired bindings");
    const QDir dir(directory.path());
    QJsonObject bindings;
    for (const auto* removed : kRetiredActions)
        bindings.insert(QString::fromLatin1(removed), QJsonArray{QStringLiteral("Ctrl+Alt+F12")});
    // The top-level "editor" opened an agent's folder, a retired command.
    const QString path = write_config(
        dir, QJsonDocument(QJsonObject{{QStringLiteral("keybindings"), bindings},
                                       {QStringLiteral("editor"), QStringLiteral("Zed")}})
                 .toJson());
    Config config;
    config.setSourcePathForTesting(path);
    require(config.load(), "a config binding retired actions loads");
    require(config.diagnostic().isEmpty(), "retired names load without complaint");
    require(config.setTheme(QStringLiteral("graphite")), "an appearance change saves");
    require(read_config(path).value(QStringLiteral("editor")).toString() == QStringLiteral("Zed"),
            "saving leaves a retired setting as written");
    require(config.sequences(QStringLiteral("quit")) == Config().sequences(QStringLiteral("quit")),
            "the other defaults are unchanged");
}

void navigation_defaults_preserve_terminal_editing() {
    Config config;
    require(config.sequences(QStringLiteral("focusLeft")).isEmpty() &&
                config.sequences(QStringLiteral("focusRight")).isEmpty(),
            "line-editing arrows must not change the focused agent");
    const QStringList actions{QStringLiteral("nextCategory"), QStringLiteral("previousCategory"),
                              QStringLiteral("nextWindow"),   QStringLiteral("previousWindow"),
                              QStringLiteral("newAgent"),     QStringLiteral("newCategory"),
                              QStringLiteral("closeAgent"),   QStringLiteral("quit")};
    require(config.sequences(QStringLiteral("detachWindow")).isEmpty(),
            "the retired detach action stays unbound");
    for (const auto* removed : {"nextPane", "zoomPane", "paneLeft", "dividerLeft"})
        require(config.sequences(QString::fromLatin1(removed)).isEmpty(),
                "the old pane actions stay retired");
    for (const auto* removed : kRetiredActions)
        require(config.sequences(QString::fromLatin1(removed)).isEmpty(),
                "retired actions have no default keys");
#ifdef Q_OS_MACOS
    require(
        config.sequences(QStringLiteral("nextCategory")) ==
                QStringList({QStringLiteral("Meta+]"), QStringLiteral("Meta+Alt+Right"),
                             QStringLiteral("Meta+Shift+Down"), QStringLiteral("Meta+Shift+J")}) &&
            config.sequences(QStringLiteral("previousCategory")) ==
                QStringList({QStringLiteral("Meta+["), QStringLiteral("Meta+Alt+Left"),
                             QStringLiteral("Meta+Shift+Up"), QStringLiteral("Meta+Shift+K")}),
        "categories take Command-[ and ], keep Command-Option-arrows and "
        "Command-Shift-arrows, and Command-Shift-J/K on the home row");
    require(config.sequences(QStringLiteral("newAgent")) == QStringList{QStringLiteral("Meta+T")} &&
                config.sequences(QStringLiteral("newCategory")) ==
                    QStringList{QStringLiteral("Meta+N")},
            "as in a browser, Command-T opens an agent and Command-N a category");
    // As in a browser, Command-W closes what is in front (and the window once
    // nothing is left) and Command-Shift-W the window; Command-M minimizes.
    require(config.sequences(QStringLiteral("closeAgent")) ==
                    QStringList{QStringLiteral("Meta+W")} &&
                config.sequences(QStringLiteral("closeWindow")) ==
                    QStringList{QStringLiteral("Meta+Shift+W")} &&
                config.sequences(QStringLiteral("minimizeWindow")) ==
                    QStringList{QStringLiteral("Meta+M")},
            "Command-W closes the agent, Command-Shift-W the window, Command-M minimizes");
    require(config.sequences(QStringLiteral("toggleTerminal")) ==
                    QStringList({QStringLiteral("Meta+`"), QStringLiteral("Ctrl+`")}) &&
                config.sequences(QStringLiteral("chooseTerminal")) ==
                    QStringList({QStringLiteral("Meta+Shift+`"), QStringLiteral("Meta+~")}) &&
                config.sequences(QStringLiteral("resumeConversation")) ==
                    QStringList{QStringLiteral("Meta+O")},
            "terminal defaults keep both grave spellings and tilde");
#else
    require(config.sequences(QStringLiteral("closeWindow")).isEmpty(),
            "without a dock to reopen it, closing the window has no key");
    require(config.sequences(QStringLiteral("minimizeWindow")).isEmpty(),
            "without a Dock, minimizing the window has no key");
    require(config.sequences(QStringLiteral("toggleTerminal")) ==
                    QStringList{QStringLiteral("Ctrl+`")} &&
                config.sequences(QStringLiteral("chooseTerminal")) ==
                    QStringList({QStringLiteral("Ctrl+Shift+~"), QStringLiteral("Ctrl+~")}) &&
                config.sequences(QStringLiteral("resumeConversation")) ==
                    QStringList{QStringLiteral("Ctrl+Shift+O")},
            "terminal defaults preserve Control chords and the resume shortcut");
    require(config.sequences(QStringLiteral("newAgent")) ==
                    QStringList{QStringLiteral("Ctrl+Shift+T")} &&
                config.sequences(QStringLiteral("newCategory")) ==
                    QStringList{QStringLiteral("Ctrl+Shift+N")},
            "Control-Shift-T opens an agent and Control-Shift-N a category");
#endif
    const QString category_modifier =
#ifdef Q_OS_MACOS
        QStringLiteral("Meta+");
#else
        QStringLiteral("Ctrl+Shift+");
#endif
    QStringList seen;
    for (int number = 1; number <= 9; ++number) {
        const auto expected = QStringList{category_modifier + QString::number(number)};
        require(config.sequences(QStringLiteral("category%1").arg(number)) == expected,
                "the first nine number chords pick the first nine categories");
        seen.append(expected.constFirst());
    }
    for (const auto& action : actions) {
        const auto sequences = config.sequences(action);
        require(!sequences.isEmpty(), "workspace actions need defaults");
        for (const auto& sequence : sequences) {
            require(!seen.contains(sequence), "workspace shortcuts must be distinct");
#ifdef Q_OS_MACOS
            require(sequence.startsWith(QStringLiteral("Meta+")),
                    "Mac workspace shortcuts must use Command");
#else
            require(sequence.startsWith(QStringLiteral("Ctrl+Shift+")),
                    "Linux workspace shortcuts must preserve terminal Control commands");
#endif
            seen.append(sequence);
        }
    }
}

// Configured text must produce a valid QKeySequence. Known actions
// retain their built-ins when a list contains no usable entry; the historical
// empty-list form does the same rather than disabling the action.
void configured_shortcuts_are_validated() {
    QTemporaryDir directory;
    require(directory.isValid(), "temporary directory for shortcut validation");
    const QDir dir(directory.path());
    const QString path = write_config(
        dir, QByteArrayLiteral(R"({"keybindings":{"nextCategory":["Nonsense","Ctrl+Alt+N",""],)"
                               R"("openSettings":["Ctrl+X, Ctrl+Y"],"custom":["Nonsense"]}})"));
    Config config;
    config.setSourcePathForTesting(path);
    require(config.load(), "mixed shortcut fixture should load");
    require(config.actionSequences(QStringLiteral("nextCategory")) ==
                QStringList{QStringLiteral("Ctrl+Alt+N")},
            "a known action should retain only valid mixed-list entries");
    require(config.actionSequences(QStringLiteral("openSettings")) == default_settings_shortcuts(),
            "settings must fall back when only a multi-chord entry is configured");
    require(config.actionSequences(QStringLiteral("custom")).isEmpty(),
            "an invalid unknown action should not expose a sequence");
    require(config.diagnostic().contains(QStringLiteral("nextCategory: invalid key sequence")),
            "invalid known actions need action-specific diagnostics");
    require(config.diagnostic().contains(
                QStringLiteral("openSettings: invalid key sequence 'Ctrl+X, Ctrl+Y'")),
            "multi-chord settings input needs a specific diagnostic");

    const QString mixed_settings_path =
        write_config(dir, QByteArrayLiteral(R"({"keybindings":{"openSettings":)"
                                            R"(["Ctrl+X, Ctrl+Y","Ctrl+Alt+S"]}})"));
    config.setSourcePathForTesting(mixed_settings_path);
    require(config.load(), "mixed settings fixture should load");
    require(config.actionSequences(QStringLiteral("openSettings")) ==
                QStringList{QStringLiteral("Ctrl+Alt+S")},
            "a valid mixed settings entry should be retained without its unusable sibling");

    const QString multi_path =
        write_config(dir, QByteArrayLiteral(R"({"keybindings":{"nextCategory":["Ctrl+X, Ctrl+Y",)"
                                            R"("Ctrl+X, Nonsense"]}})"));
    config.setSourcePathForTesting(multi_path);
    require(config.load(), "ordinary multi-chord fixture should load");
    require(config.actionSequences(QStringLiteral("nextCategory")) ==
                QStringList{QStringLiteral("Ctrl+X, Ctrl+Y")},
            "ordinary shortcuts support multiple chords, each of which must be valid");

    const QString empty_path =
        write_config(dir, QByteArrayLiteral(R"({"keybindings":{"nextCategory":[]}})"));
    config.setSourcePathForTesting(empty_path);
    require(config.load(), "empty shortcut fixture should load");
    require(!config.actionSequences(QStringLiteral("nextCategory")).isEmpty(),
            "explicit empty lists historically retain built-in bindings");
}

// Unknown names are rejected without disturbing the current selection, so a bad
// config edit or a stale dialog cannot leave the window in an undefined state.
void unknown_names_are_rejected() {
    QTemporaryDir directory;
    require(directory.isValid(), "temporary directory");
    const QDir dir(directory.path());
    const QString path = write_config(dir, R"({"version": 1})");

    Config config;
    config.setSourcePathForTesting(path);
    require(config.load(), "config should load");
    const QString theme = config.themeName();
    const QString density = config.densityName();

    require(!config.setTheme(QStringLiteral("chartreuse")), "unknown theme must be rejected");
    require(!config.setDensity(QStringLiteral("enormous")), "unknown density must be rejected");
    require(config.themeName() == theme, "a rejected theme must not change the selection");
    require(config.densityName() == density, "a rejected density must not change the selection");
    require(!config.diagnostic().isEmpty(), "a rejected name should explain itself");
}

// The loader tolerates a malformed file by keeping defaults, which is what makes
// a hand-edited config safe to ship.
void malformed_values_fall_back() {
    QTemporaryDir directory;
    require(directory.isValid(), "temporary directory");
    const QDir dir(directory.path());
    const QString path =
        write_config(dir, R"({"layout": "diagonal", "theme": "vaporwave", "density": "gigantic"})");

    Config config;
    config.setSourcePathForTesting(path);
    require(config.load(), "a syntactically valid file should still load");
    require(config.themeName() == QStringLiteral("lapis"), "unknown theme should fall back");
    require(config.densityName() == QStringLiteral("comfortable"),
            "unknown density should fall back");
    require(!config.diagnostic().isEmpty(), "falling back should be reported");
}

// harnessUpdates turns off updates for the CLIs set to false; true, absent
// and malformed entries leave a CLI updating, and malformed ones are reported.
void harness_updates_can_be_turned_off() {
    QTemporaryDir directory;
    require(directory.isValid(), "temporary directory");
    const QString path = write_config(
        QDir(directory.path()),
        R"({"harnessUpdates": {"OMP": false, "claude": true, "grok": "no", "": false}})");
    Config config;
    config.setSourcePathForTesting(path);
    require(config.load(), "harness updates load with the rest of the file");
    require(config.harnessUpdatesOff() == QSet<QString>{QStringLiteral("omp")},
            "only a CLI set to false is pinned");
    require(config.diagnostic().contains(QStringLiteral("harnessUpdates for 'grok'")),
            "a non-boolean entry is reported");
    require(config.diagnostic().contains(QStringLiteral("at least one and at most 32 characters")),
            "an empty harness name is reported");
    require(config.setSetting(QStringLiteral("harnessUpdates.omp"), true),
            "a normalized CLI name can be enabled");
    const auto saved_updates = read_config(path).value(QStringLiteral("harnessUpdates")).toObject();
    bool omp_saved = false;
    for (auto it = saved_updates.begin(); it != saved_updates.end(); ++it)
        omp_saved = omp_saved || it.key().compare(QStringLiteral("omp"), Qt::CaseInsensitive) == 0;
    require(!omp_saved, "enabling removes its differently-cased key");
    const QString defaults = write_config(QDir(directory.path()), R"({"theme": "oled"})");
    config.setSourcePathForTesting(defaults);
    require(config.load() && config.harnessUpdatesOff().isEmpty(), "every CLI updates by default");
}

// Per-harness launch arguments are literal lists; malformed entries are
// reported and dropped without affecting the valid ones or the rest of the file.
void harness_arguments_are_literal_lists() {
    QTemporaryDir directory;
    require(directory.isValid(), "temporary directory");
    const QDir dir(directory.path());
    const QString path = write_config(
        QDir(directory.path()),
        R"({"theme": "oled", "harnessArguments": {"claude": ["--dangerously-skip-permissions"],)"
        R"( "grok": "--yolo", "kimi": ["", 3]}})");
    Config config;
    config.setSourcePathForTesting(path);
    require(config.load(), "harness arguments load with the rest of the file");
    require(config.harnessArguments().value(QStringLiteral("claude")) ==
                QStringList{QStringLiteral("--dangerously-skip-permissions")},
            "a list of strings is kept literally");
    require(!config.harnessArguments().contains(QStringLiteral("grok")) &&
                !config.harnessArguments().contains(QStringLiteral("kimi")),
            "a string or a list with empty or non-string items is dropped");
    require(config.diagnostic().contains(QStringLiteral("harnessArguments")),
            "dropped entries are reported");
    require(config.themeName() == QStringLiteral("oled"), "other settings still apply");
    require(config.setTheme(QStringLiteral("graphite")), "persist an appearance change");
    require(read_config(path)
                    .value(QStringLiteral("harnessArguments"))
                    .toObject()
                    .value(QStringLiteral("claude"))
                    .toArray()
                    .size() == 1,
            "saving appearance keeps the harness arguments");

    const QString long_name(40, QLatin1Char('n'));
    const QString invalid_name = write_config(
        dir,
        QStringLiteral(R"({"harnessArguments":{"%1":["--qualified"]}})").arg(long_name).toUtf8());
    Config overlong_name;
    overlong_name.setSourcePathForTesting(invalid_name);
    require(overlong_name.load(), "an overlong harness name still loads other defaults");
    require(overlong_name.harnessArguments().isEmpty(), "an overlong harness name is dropped");
    require(overlong_name.diagnostic().contains(QStringLiteral("at most 32 characters")),
            "the diagnostic names the harness-name bound");

    const QString long_argument = QStringLiteral("--%1").arg(QString(1025, QLatin1Char('x')));
    const QString invalid_argument = write_config(
        dir,
        QStringLiteral(R"({"harnessArguments":{"claude":["%1"]}})").arg(long_argument).toUtf8());
    Config overlong_argument;
    overlong_argument.setSourcePathForTesting(invalid_argument);
    require(overlong_argument.load(), "an overlong argument still loads other defaults");
    require(overlong_argument.harnessArguments().isEmpty(), "an overlong argument is dropped");
    require(overlong_argument.diagnostic().contains(QStringLiteral("at most 1024 characters")),
            "the diagnostic names the argument bound");

    const QString empty_name =
        write_config(dir, R"({"harnessArguments":{"":["--dropped"],"claude":["--kept"]}})");
    Config empty_key;
    empty_key.setSourcePathForTesting(empty_name);
    require(empty_key.load(), "an empty harness key still loads the rest of the file");
    require(!empty_key.harnessArguments().contains(QString()) &&
                empty_key.harnessArguments().value(QStringLiteral("claude")) ==
                    QStringList{QStringLiteral("--kept")},
            "an empty harness key is dropped without affecting a valid key");
    require(empty_key.diagnostic().contains(QStringLiteral("Ignoring harnessArguments for ''")),
            "an empty harness key is reported");
}

// The dialog builds its controls from these lists, so they must agree with the
// names the setters accept.
void advertised_names_are_accepted() {
    QTemporaryDir directory;
    require(directory.isValid(), "temporary directory");
    const QDir dir(directory.path());
    const QString path = write_config(dir, R"({"version": 1})");

    Config config;
    config.setSourcePathForTesting(path);
    require(config.load(), "config should load");

    for (const QString& name : config.densities())
        require(config.setDensity(name), "every advertised density must be accepted");
    const QVariantList themes = config.themes();
    require(!themes.isEmpty(), "the dialog needs at least one theme");
    for (const QVariant& entry : themes) {
        const QString name = entry.toMap().value(QStringLiteral("name")).toString();
        require(!name.isEmpty(), "each advertised theme needs a name");
        require(config.setTheme(name), "every advertised theme must be accepted");
        // Category names are written in their colour, full for the shown
        // group and quieter for the rest: both keep text contrast (4.5:1)
        // on the sidebar's surface and the window background, and the full
        // colour's lines and edges keep a mark's 3:1 on cards too.
        const auto map = entry.toMap();
        const auto colors = map.value(QStringLiteral("categoryColors")).toMap();
        const auto quiet = map.value(QStringLiteral("categoryQuietColors")).toMap();
        require(colors.size() == 8 && quiet.keys() == colors.keys(),
                "every theme resolves the eight category colours and their quiet names");
        const auto luminance = [](const QColor& color) {
            const auto channel = [](double value) {
                return value <= 0.03928 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
            };
            return 0.2126 * channel(color.redF()) + 0.7152 * channel(color.greenF()) +
                   0.0722 * channel(color.blueF());
        };
        const auto contrast = [&](const QVariant& color, const char* surface) {
            const auto ground =
                luminance(QColor(map.value(QString::fromLatin1(surface)).toString()));
            const auto ink = luminance(QColor(color.toString()));
            return (std::max(ink, ground) + 0.05) / (std::min(ink, ground) + 0.05);
        };
        for (auto hue = colors.cbegin(); hue != colors.cend(); ++hue) {
            for (const auto* surface : {"surface", "background"}) {
                require(contrast(hue.value(), surface) >= 4.5,
                        "category names keep text contrast on every theme");
                require(contrast(quiet.value(hue.key()), surface) >= 4.5,
                        "quiet category names keep text contrast on every theme");
            }
            require(contrast(hue.value(), "card") >= 3.0,
                    "category colours keep 3:1 contrast on cards");
            require(quiet.value(hue.key()) != hue.value(),
                    "a quiet name differs from the full one");
        }
    }
    // The final iteration leaves the last advertised name selected, and the
    // enumeration agrees with the string names written to the file.
    require(config.densityName() == QStringLiteral("minimal"),
            "last advertised density is minimal");
    require(static_cast<int>(CardDensity::Comfortable) == 0, "density order is part of the file");
}

void sidebar_preference_and_commands() {
    QTemporaryDir directory;
    require(directory.isValid(), "sidebar fixture directory");
    const auto path = write_config(QDir(directory.path()), R"({"retained":true})");
    Config config;
    config.setSourcePathForTesting(path);
    require(config.load() && config.sidebarVisible(), "sidebar defaults visible");
    // The sidebar navigates; the preview strip is opt-in.
    require(!config.previewsVisible(), "previews default hidden");
    QList<QPair<bool, bool>> observed;
    QObject::connect(&config, &Config::changed, &config,
                     [&] { observed.append({config.sidebarVisible(), config.previewsVisible()}); });
    require(config.setSidebarVisible(false), "hide sidebar persists");
    require(config.setPreviewsVisible(true), "show previews persists");
    require(observed == QList<QPair<bool, bool>>{{false, false}, {false, true}},
            "successful visibility saves emit each settled state once");
    Config restored;
    restored.setSourcePathForTesting(path);
    require(restored.load() && !restored.sidebarVisible(), "hidden sidebar survives restart");
    require(read_config(path).value(QStringLiteral("retained")).toBool(),
            "sidebar preserves unrelated settings");
#ifdef Q_OS_MACOS
    const auto paste = QKeySequence(QStringLiteral("Meta+V"))[0];
#else
    const auto paste = QKeySequence(QStringLiteral("Ctrl+Shift+V"))[0];
#endif
    for (const auto& action : {QStringLiteral("toggleSidebar"), QStringLiteral("openCommands")}) {
        const auto bindings = config.sequences(action);
        require(!bindings.isEmpty(), "new workspace actions have shortcuts");
        for (const auto& binding : bindings)
            require(QKeySequence(binding)[0] != paste, "workspace shortcuts must not steal paste");
    }
    require(restored.previewsVisible(), "shown previews survive restart");
    observed.clear();
    QObject::connect(&restored, &Config::changed, &restored, [&] {
        observed.append({restored.sidebarVisible(), restored.previewsVisible()});
    });
    static_cast<void>(write_config(QDir(directory.path()), "{broken"));
    require(!restored.setSidebarVisible(true) && !restored.sidebarVisible(),
            "failed sidebar save rolls back");
    require(!restored.setPreviewsVisible(false) && restored.previewsVisible(),
            "failed preview save rolls back");
    require(observed == QList<QPair<bool, bool>>{{false, true}, {false, true}},
            "failed visibility saves never publish tentative values");
    // A file an earlier lapis wrote with either value keeps it.
    for (const bool shown : {true, false}) {
        const auto earlier =
            write_config(QDir(directory.path()),
                         shown ? R"({"previewsVisible": true})" : R"({"previewsVisible": false})");
        Config kept;
        kept.setSourcePathForTesting(earlier);
        require(kept.load() && kept.previewsVisible() == shown,
                "an explicit previewsVisible keeps its value");
    }
}

void typing_scroll_preference_persists() {
    QTemporaryDir directory;
    require(directory.isValid(), "typing scroll fixture directory");
    const auto path = write_config(QDir(directory.path()), R"({})");
    Config config;
    config.setSourcePathForTesting(path);
    require(config.load() && config.scrollToBottomOnTyping(),
            "typing returns to the bottom by default");
    require(config.setScrollToBottomOnTyping(false), "the typing return can be turned off");
    require(!read_config(path).value(QStringLiteral("scrollToBottomOnTyping")).toBool(true),
            "the typing return is not persisted");
    Config restored;
    restored.setSourcePathForTesting(path);
    require(restored.load() && !restored.scrollToBottomOnTyping(),
            "the typing return preference survives restart");
    static_cast<void>(write_config(QDir(directory.path()), "{broken"));
    require(!restored.setScrollToBottomOnTyping(true) && !restored.scrollToBottomOnTyping(),
            "a failed typing return save rolls back");
    require(!restored.load() && restored.scrollToBottomOnTyping(),
            "an unparsable file resets the typing return to its default");
}

// The visual view's theme, fonts, size and all-agents switch are saved under
// "visual"; bad values are refused and hand-edited ones fall back.
void visual_settings_persist() {
    QTemporaryDir directory;
    require(directory.isValid(), "visual fixture directory");
    const auto path = write_config(QDir(directory.path()), R"({})");
    Config config;
    config.setSourcePathForTesting(path);
    require(config.load() &&
                config.visual().value(QStringLiteral("theme")) == QStringLiteral("terminal") &&
                config.visual().value(QStringLiteral("size")).toInt() == 0 &&
                !config.visual().value(QStringLiteral("all")).toBool() &&
                config.visual().value(QStringLiteral("editor")) == QStringLiteral("auto"),
            "visual defaults follow the terminal; files open in the first editor installed");
    require(config.setVisual(QStringLiteral("editor"), QStringLiteral("cursor")) &&
                read_config(path)
                        .value(QStringLiteral("visual"))
                        .toObject()
                        .value(QStringLiteral("editor")) == QStringLiteral("cursor") &&
                !config.setVisual(QStringLiteral("editor"), QStringLiteral("Bad Name")) &&
                !config.setVisual(QStringLiteral("sendOnReturn"), true) &&
                config.setVisual(QStringLiteral("editor"), QStringLiteral("auto")),
            "the editor choice saves; Return-sends is gone");
    require(config.setVisual(QStringLiteral("theme"), QStringLiteral("claude")) &&
                config.setVisual(QStringLiteral("font"), QStringLiteral("serif")) &&
                config.setVisual(QStringLiteral("size"), 18) &&
                config.setVisual(QStringLiteral("all"), true),
            "visual choices save");
    require(!config.setVisual(QStringLiteral("size"), 4) &&
                !config.setVisual(QStringLiteral("theme"), QStringLiteral("Bad Name")) &&
                !config.setVisual(QStringLiteral("unknown"), 1),
            "bad visual values are refused");
    const auto saved = read_config(path).value(QStringLiteral("visual")).toObject();
    require(saved.value(QStringLiteral("theme")).toString() == QStringLiteral("claude") &&
                saved.value(QStringLiteral("size")).toInt() == 18 &&
                saved.value(QStringLiteral("all")).toBool(),
            "visual choices are written");
    Config restored;
    restored.setSourcePathForTesting(path);
    require(restored.load() &&
                restored.visual().value(QStringLiteral("font")) == QStringLiteral("serif") &&
                restored.visual().value(QStringLiteral("size")).toInt() == 18,
            "visual choices survive restart");
    static_cast<void>(write_config(QDir(directory.path()),
                                   R"({"visual":{"theme":"../x","size":200,"all":"yes"}})"));
    require(restored.load() &&
                restored.visual().value(QStringLiteral("theme")) == QStringLiteral("terminal") &&
                restored.visual().value(QStringLiteral("size")).toInt() == 0 &&
                !restored.visual().value(QStringLiteral("all")).toBool(),
            "hand-edited visual values fall back");
}

// visual.keepMB and visual.keepThreads are hand-written: whole numbers from
// 0 to their limits (65536 MB, 1000 threads) are taken, and anything else
// (missing, negative, too large, fractional or not a number) keeps the
// default of 1024 MB and 32 threads.
void visual_keep_budget_parses() {
    QTemporaryDir directory;
    require(directory.isValid(), "keep budget fixture directory");
    const QDir dir(directory.path());
    Config config;
    config.setSourcePathForTesting(write_config(dir, R"({})"));
    require(config.load() && config.visualKeepMB() == 1024 && config.visualKeepThreads() == 32,
            "the keep budget defaults to 1024 MB and 32 threads");
    struct Case {
        const char* name{};
        QByteArray visual;
        int mb{};
        int threads{};
    };
    const std::array cases{
        Case{"in range", R"({"keepMB": 512, "keepThreads": 8})", 512, 8},
        Case{"zero keeps none", R"({"keepMB": 0, "keepThreads": 0})", 0, 0},
        Case{"the limits", R"({"keepMB": 65536, "keepThreads": 1000})", 65536, 1000},
        Case{"past the limits", R"({"keepMB": 65537, "keepThreads": 1001})", 1024, 32},
        Case{"negative", R"({"keepMB": -1, "keepThreads": -5})", 1024, 32},
        Case{"fractional", R"({"keepMB": 1.5, "keepThreads": 2.5})", 1024, 32},
        Case{"strings", R"({"keepMB": "512", "keepThreads": "8"})", 1024, 32},
        Case{"booleans and null", R"({"keepMB": true, "keepThreads": null})", 1024, 32},
        Case{"huge", R"({"keepMB": 1e300, "keepThreads": 4294967296})", 1024, 32},
        Case{"one bad, one good", R"({"keepMB": "lots", "keepThreads": 4})", 1024, 4},
    };
    for (const auto& c : cases) {
        static_cast<void>(write_config(dir, R"({"visual":)" + c.visual + "}"));
        if (!config.load() || config.visualKeepMB() != c.mb ||
            config.visualKeepThreads() != c.threads) {
            std::cerr << "visual keep case \"" << c.name << "\": keepMB " << config.visualKeepMB()
                      << ", keepThreads " << config.visualKeepThreads() << '\n';
            require(false, "visual.keepMB and visual.keepThreads parse as documented");
        }
    }
    // A reload without them goes back to the defaults.
    static_cast<void>(write_config(dir, R"({"visual":{"theme":"terminal"}})"));
    require(config.load() && config.visualKeepMB() == 1024 && config.visualKeepThreads() == 32,
            "a reload without the keys restores the defaults");
}

// Golden file for Settings: every setting it changes lands on its own
// lapis.json key, beside hand-written keys left as they were.
void settings_write_known_keys() {
    QTemporaryDir directory;
    require(directory.isValid(), "golden fixture directory");
    const auto path = write_config(
        QDir(directory.path()),
        R"({"keybindings":{"quit":["Meta+Q"]},"custom":1,)"
        R"("newAgent":{"machines":{"box":{"folder":"~/w"}},"models":{"codex":["m1"]}},)"
        R"("nextPrompt":{"model":"x","providers":[]}})");
    Config config;
    config.setSourcePathForTesting(path);
    config.setSystemDark(true);
    require(config.load(), "golden fixture loads");
    auto concurrent = read_config(path);
    auto concurrent_agents = concurrent.value(QStringLiteral("newAgent")).toObject();
    concurrent_agents.insert(
        QStringLiteral("models"),
        QJsonObject{{QStringLiteral("codex"), QJsonArray{QStringLiteral("external")}}});
    concurrent.insert(QStringLiteral("newAgent"), concurrent_agents);
    static_cast<void>(write_config(QDir(directory.path()),
                                   QJsonDocument(concurrent).toJson(QJsonDocument::Compact)));
    const auto set = [&config](const char* key, const QVariant& value) {
        require(config.setSetting(QString::fromLatin1(key), value), key);
    };
    require(config.setTheme(QStringLiteral("graphite")) &&
                config.setDensity(QStringLiteral("compact")) && config.setSidebarVisible(false) &&
                config.setPreviewsVisible(false) && config.setScrollToBottomOnTyping(false) &&
                config.setTerminalFontFamily(QStringLiteral("Menlo")) &&
                config.setTerminalFontSize(16) && config.setFinishSound(false) &&
                config.setKeepAwake(false),
            "window settings save");
    require(config.setVisual(QStringLiteral("font"), QStringLiteral("serif")) &&
                config.setVisual(QStringLiteral("codeFont"), QStringLiteral("menlo")) &&
                config.setVisual(QStringLiteral("size"), 18) &&
                config.setVisual(QStringLiteral("all"), true) &&
                config.setVisual(QStringLiteral("editor"), QStringLiteral("zed")) &&
                config.setVisual(QStringLiteral("theme"), QStringLiteral("tokyo-night")),
            "reading view settings save");
    set("newAgent.harness", QStringLiteral("codex"));
    set("newAgent.folder", QStringLiteral("~/dev"));
    set("newAgent.mode", QStringLiteral("edits"));
    set("harnessUpdates.omp", false);
    set("nextPrompt.auto", true);
    set("interactionLog.enabled", true);
    set("tabAway.overdueHours", 6);
    const auto expected = QJsonDocument::fromJson(R"({
        "keybindings": {"quit": ["Meta+Q"]},
        "custom": 1,
        "newAgent": {"machines": {"box": {"folder": "~/w"}}, "models": {"codex": ["external"]},
                     "harness": "codex", "folder": "~/dev", "mode": "edits"},
        "nextPrompt": {"model": "x", "providers": [], "auto": true},
        "harnessUpdates": {"omp": false},
        "interactionLog": {"enabled": true},
        "tabAway": {"overdueHours": 6},
        "theme": "graphite",
        "density": "compact",
        "sidebarVisible": false,
        "previewsVisible": false,
        "scrollToBottomOnTyping": false,
        "terminalFont": {"size": 16, "family": "Menlo"},
        "alerts": {"finished": false},
        "keepAwake": false,
        "visual": {"theme": "tokyo-night", "font": "serif", "codeFont": "menlo", "size": 18,
                   "all": true, "editor": "zed",
                   "darkTheme": "tokyo-night", "lightTheme": "claude-light",
                   "darkWindowTheme": "graphite", "lightWindowTheme": "daylight"},
        "version": 1
    })")
                              .object();
    require(read_config(path) == expected, "the saved file matches the golden file");
    const auto merged_defaults = config.agentDefaults();
    require(merged_defaults.models.value(QStringLiteral("codex")) ==
                QStringList{QStringLiteral("external")},
            "a merged section also updates the running configuration");
    require(config.agentDefaults().harness == QStringLiteral("codex") &&
                config.agentDefaults().machineFolders.value(QStringLiteral("box")) ==
                    QStringLiteral("~/w") &&
                config.harnessUpdatesOff().contains(QStringLiteral("omp")) &&
                config.nextPrompt().automatic && config.nextPrompt().model == QStringLiteral("x") &&
                config.interactionLog().enabled && config.tabAway().overdueHours == 6,
            "the settings apply at once");

    // Turning updates back on removes the key rather than writing true;
    // bad values are refused.
    set("harnessUpdates.omp", true);
    require(read_config(path).value(QStringLiteral("harnessUpdates")).toObject().isEmpty(),
            "updating again leaves no key");
    require(!config.setSetting(QStringLiteral("newAgent.mode"), QStringLiteral("yolo")) &&
                !config.setSetting(QStringLiteral("tabAway.overdueHours"), -1) &&
                !config.setSetting(QStringLiteral("nextPrompt.auto"), QStringLiteral("yes")) &&
                !config.setSetting(QStringLiteral("nextPrompt.model"), QStringLiteral("y")) &&
                !config.setSetting(QStringLiteral("harnessUpdates.Bad Name"), false),
            "unknown paths and bad values are refused");
    set("newAgent.folder", QString());
    require(!read_config(path)
                 .value(QStringLiteral("newAgent"))
                 .toObject()
                 .contains(QStringLiteral("folder")),
            "an empty folder removes the key");

    // A section Settings did not just change is never rewritten from memory:
    // an edit made since stays.
    auto edited = read_config(path);
    auto next = edited.value(QStringLiteral("nextPrompt")).toObject();
    next.insert(QStringLiteral("maxPerHour"), 5);
    edited.insert(QStringLiteral("nextPrompt"), next);
    static_cast<void>(
        write_config(QDir(directory.path()), QJsonDocument(edited).toJson(QJsonDocument::Compact)));
    require(config.setKeepAwake(true), "an unrelated save");
    require(read_config(path)
                    .value(QStringLiteral("nextPrompt"))
                    .toObject()
                    .value(QStringLiteral("maxPerHour"))
                    .toInt() == 5,
            "an untouched section stays as edited");
    const auto defaults = Config::defaults();
    require(defaults.value(QStringLiteral("theme")) == QStringLiteral("lapis") &&
                defaults.value(QStringLiteral("visual.appearance")) == QStringLiteral("dark") &&
                defaults.value(QStringLiteral("terminalFont.size")).toInt() == 14 &&
                defaults.value(QStringLiteral("tabAway.overdueHours")).toDouble() == 4 &&
                !defaults.value(QStringLiteral("nextPrompt.auto")).toBool(),
            "built-in values for Reset");
}

// Light mode: one switch moves the thread and the window to the person's
// last theme of the other kind, and back; "system" follows macOS live
// without writing the file; a file from before it shows what it did.
void appearance_switches_kinds() {
    QTemporaryDir directory;
    require(directory.isValid(), "appearance fixture directory");
    const auto path = write_config(QDir(directory.path()), R"({"theme":"oled"})");
    Config config;
    config.setSourcePathForTesting(path);
    config.setSystemDark(true);
    const auto shown = [&config] {
        return config.themeName() + QLatin1Char('/') +
               config.visual().value(QStringLiteral("theme")).toString();
    };
    require(config.load() && config.appearance() == QStringLiteral("dark") &&
                shown() == QStringLiteral("oled/terminal") && !config.lightShown(),
            "a file without the mode stays dark as it was");
    require(config.setAppearance(QStringLiteral("light")) &&
                shown() == QStringLiteral("daylight/claude-light") && config.lightShown(),
            "light mode shows the light window and thread");
    auto saved = read_config(path);
    auto visual = saved.value(QStringLiteral("visual")).toObject();
    require(visual.value(QStringLiteral("appearance")) == QStringLiteral("light") &&
                visual.value(QStringLiteral("darkWindowTheme")) == QStringLiteral("oled") &&
                visual.value(QStringLiteral("lightWindowTheme")) == QStringLiteral("daylight") &&
                visual.value(QStringLiteral("darkTheme")) == QStringLiteral("terminal") &&
                visual.value(QStringLiteral("lightTheme")) == QStringLiteral("claude-light") &&
                saved.value(QStringLiteral("theme")) == QStringLiteral("daylight") &&
                visual.value(QStringLiteral("theme")) == QStringLiteral("claude-light"),
            "the mode and each kind's theme are saved under visual");
    require(config.setVisual(QStringLiteral("theme"), QStringLiteral("codex-light")) &&
                config.appearance() == QStringLiteral("light"),
            "a light theme in light mode stays light");
    require(config.setVisual(QStringLiteral("theme"), QStringLiteral("tokyo-night")) &&
                config.appearance() == QStringLiteral("dark") &&
                shown() == QStringLiteral("oled/tokyo-night"),
            "a dark theme switches to dark mode, window included");
    require(config.setTheme(QStringLiteral("graphite")) &&
                shown() == QStringLiteral("graphite/tokyo-night"),
            "a dark window theme is the dark one");
    require(config.setAppearance(QStringLiteral("light")) &&
                shown() == QStringLiteral("daylight/codex-light"),
            "light mode returns to the last light themes");
    require(config.setAppearance(QStringLiteral("dark")) &&
                shown() == QStringLiteral("graphite/tokyo-night"),
            "dark mode returns to the last dark themes");
    require(!config.setAppearance(QStringLiteral("dim")) &&
                config.appearance() == QStringLiteral("dark"),
            "an unknown mode is refused");
    Config restored;
    restored.setSourcePathForTesting(path);
    restored.setSystemDark(true);
    require(restored.load() && restored.themeName() == QStringLiteral("graphite") &&
                restored.visual().value(QStringLiteral("lightTheme")) ==
                    QStringLiteral("codex-light"),
            "both kinds survive a restart");

    // Match macOS follows the platform's appearance as it changes, unsaved.
    require(config.setAppearance(QStringLiteral("system")) && !config.lightShown(),
            "system while macOS is dark");
    const auto before = read_config(path);
    config.setSystemDark(false);
    require(config.lightShown() && shown() == QStringLiteral("daylight/codex-light"),
            "macOS turning light shows light mode");
    config.setSystemDark(true);
    require(shown() == QStringLiteral("graphite/tokyo-night"), "and dark again");
    require(read_config(path) == before, "following macOS writes nothing");
    config.setSystemDark(false);
    require(config.setTheme(QStringLiteral("lapis")) &&
                config.appearance() == QStringLiteral("dark") &&
                shown() == QStringLiteral("lapis/tokyo-night"),
            "a dark theme chosen while macOS is light switches to dark for good");
    Config system;
    system.setSourcePathForTesting(
        write_config(QDir(directory.path()),
                     R"({"visual":{"appearance":"system","lightTheme":"codex-light"}})"));
    system.setSystemDark(false);
    require(system.load() &&
                system.visual().value(QStringLiteral("theme")) == QStringLiteral("codex-light"),
            "a saved system mode loads as macOS shows");

    // A file from before light mode that already read in a light theme.
    static_cast<void>(write_config(QDir(directory.path()),
                                   R"({"theme":"oled","visual":{"theme":"claude-light"}})"));
    Config mixed;
    mixed.setSourcePathForTesting(path);
    require(mixed.load() && mixed.appearance() == QStringLiteral("light") &&
                mixed.themeName() == QStringLiteral("oled") &&
                mixed.visual().value(QStringLiteral("theme")) == QStringLiteral("claude-light"),
            "an older light thread in a dark window shows as it did");
    require(mixed.save(), "rewriting an older mixed file succeeds");
    saved = read_config(path);
    visual = saved.value(QStringLiteral("visual")).toObject();
    require(!visual.contains(QStringLiteral("appearance")) &&
                saved.value(QStringLiteral("theme")) == QStringLiteral("oled") &&
                visual.value(QStringLiteral("theme")) == QStringLiteral("claude-light"),
            "an unchosen mode is not invented by a save");
    Config legacy_reloaded;
    legacy_reloaded.setSourcePathForTesting(path);
    require(legacy_reloaded.load() && legacy_reloaded.themeName() == QStringLiteral("oled") &&
                legacy_reloaded.visual().value(QStringLiteral("theme")) ==
                    QStringLiteral("claude-light"),
            "an older mixed appearance survives its own rewrite");
    const auto daylight_path = write_config(QDir(directory.path()), R"({"theme":"daylight"})");
    Config legacy_light_window;
    legacy_light_window.setSourcePathForTesting(daylight_path);
    require(legacy_light_window.load() &&
                legacy_light_window.themeName() == QStringLiteral("daylight") &&
                legacy_light_window.appearance() == QStringLiteral("dark"),
            "an older light window with a dark thread shows as it did");
    require(legacy_light_window.save() && legacy_light_window.load() &&
                legacy_light_window.themeName() == QStringLiteral("daylight") &&
                !read_config(daylight_path)
                     .value(QStringLiteral("visual"))
                     .toObject()
                     .contains(QStringLiteral("appearance")),
            "rewriting an older light window does not replace it");
    require(legacy_light_window.setAppearance(QStringLiteral("dark")) &&
                legacy_light_window.themeName() == QStringLiteral("lapis"),
            "choosing the inferred mode applies it at once");
    require(read_config(daylight_path)
                    .value(QStringLiteral("visual"))
                    .toObject()
                    .value(QStringLiteral("appearance"))
                    .toString() == QStringLiteral("dark"),
            "the first explicit choice is saved");
    require(mixed.setAppearance(QStringLiteral("dark")) &&
                mixed.themeName() == QStringLiteral("oled") &&
                mixed.visual().value(QStringLiteral("theme")) == QStringLiteral("terminal"),
            "and switches to dark with its own window theme kept");
    static_cast<void>(write_config(
        QDir(directory.path()),
        R"({"visual":{"appearance":"light","lightTheme":"tokyo-night","lightWindowTheme":"oled"}})"));
    require(mixed.load() && mixed.themeName() == QStringLiteral("daylight") &&
                mixed.visual().value(QStringLiteral("theme")) == QStringLiteral("claude-light"),
            "a theme of the wrong kind in a slot falls back");
}

// The terminal font is validated, written immediately, restored on restart and
// rolled back when the file cannot be saved.
void terminal_font_persists_and_rolls_back() {
    QTemporaryDir directory;
    require(directory.isValid(), "font fixture directory");
    const QDir dir(directory.path());
    const QString path =
        write_config(dir, R"({"retained":true,"terminalFont":{"size":20,"ligatures":false}})");
    Config config;
    config.setSourcePathForTesting(path);
    require(config.load(), "font fixture loads");
    require(config.terminalFontFamily().isEmpty(), "font family defaults to the system font");
    require(config.terminalFontSize() == 20, "the explicit non-default font size is loaded");

    require(config.setTerminalFontSize(22), "valid font size applies");
    require(config.setTerminalFontFamily(QStringLiteral("  Test Mono  ")),
            "font family applies with surrounding space trimmed");
    require(config.terminalFontFamily() == QStringLiteral("Test Mono"), "trimmed family stored");
    QJsonObject font = read_config(path).value(QStringLiteral("terminalFont")).toObject();
    require(font.value(QStringLiteral("size")).toInt() == 22, "font size written");
    require(font.value(QStringLiteral("family")).toString() == QStringLiteral("Test Mono"),
            "font family written");
    require(!font.value(QStringLiteral("ligatures")).toBool(true),
            "unrelated terminalFont keys survive a write");
    require(read_config(path).value(QStringLiteral("retained")).toBool(),
            "font save preserves unrelated settings");

    Config restored;
    restored.setSourcePathForTesting(path);
    require(restored.load(), "saved font config reloads");
    require(restored.terminalFontSize() == 22 &&
                restored.terminalFontFamily() == QStringLiteral("Test Mono"),
            "font survives a restart");

    require(!config.setTerminalFontSize(lapis::desktop::kTerminalFontSizeMinimum - 1),
            "below-minimum font sizes are rejected");
    require(!config.setTerminalFontSize(lapis::desktop::kTerminalFontSizeMaximum + 1),
            "above-maximum font sizes are rejected");
    require(!config.setTerminalFontFamily(QStringLiteral("Bad\nName")),
            "control characters in a family name are rejected");
    require(!config.setTerminalFontFamily(QString(129, QLatin1Char('M'))),
            "an over-length family name is rejected");
    require(config.terminalFontSize() == 22 &&
                config.terminalFontFamily() == QStringLiteral("Test Mono"),
            "rejected font values do not change the selection");

    require(config.setTerminalFontFamily(QString()), "empty family restores the system font");
    font = read_config(path).value(QStringLiteral("terminalFont")).toObject();
    require(!font.contains(QStringLiteral("family")), "the default family is not written");

    const QByteArray malformed = "{unfinished user edit";
    static_cast<void>(write_config(dir, malformed));
    require(!config.setTerminalFontSize(24), "font size save fails on a malformed file");
    require(config.terminalFontSize() == 22, "failed font size save rolls back");
    require(!config.setTerminalFontFamily(QStringLiteral("Other Mono")),
            "font family save fails on a malformed file");
    require(config.terminalFontFamily().isEmpty(), "failed font family save rolls back");
    require(config.diagnostic().contains(QStringLiteral("Could not save")),
            "failed font save is visible");
    QFile raw(path);
    require(raw.open(QIODevice::ReadOnly), "read malformed font config");
    require(raw.readAll() == malformed, "failed font save preserves malformed bytes");
    raw.close();

    for (const auto* value : {"null", "42", "[1,2,3]", "\"system mono\""}) {
        const QByteArray contents = QByteArray("{\"terminalFont\":") + value + "}";
        static_cast<void>(write_config(dir, contents));
        const bool previous = config.sidebarVisible();
        require(!config.setSidebarVisible(!previous) && config.sidebarVisible() == previous,
                "appearance save rolls back instead of replacing a malformed font value");
        require(config.diagnostic().contains(QStringLiteral("terminalFont must be an object")),
                "malformed font save explains the rejected update");
        require(raw.open(QIODevice::ReadOnly), "read malformed font value");
        require(raw.readAll() == contents, "malformed font value remains byte-for-byte intact");
        raw.close();
    }

    const QString invalid =
        write_config(dir, R"({"terminalFont":{"family":"Tab\tName","size":16.5}})");
    Config fallback;
    fallback.setSourcePathForTesting(invalid);
    require(fallback.load(), "invalid font values still load the file");
    require(fallback.terminalFontFamily().isEmpty() &&
                fallback.terminalFontSize() == lapis::desktop::kTerminalFontSizeDefault,
            "invalid font values fall back to defaults");
    require(fallback.diagnostic().contains(QStringLiteral("terminalFont.family")) &&
                fallback.diagnostic().contains(QStringLiteral("terminalFont.size")),
            "each invalid font value is reported");
}

// Remote settings are one Config-owned transaction, not three separately
// persisted setters. A failed save leaves both memory and disk where they were.
void remote_settings_are_transactional() {
    QTemporaryDir directory;
    require(directory.isValid(), "remote-settings fixture directory");
    const QDir dir(directory.path());
    const QString path = write_config(dir, R"({"retained":true})");
    Config config;
    config.setSourcePathForTesting(path);
    require(config.load(), "remote-settings fixture loads");

    int changes = 0;
    QObject::connect(&config, &Config::changed, [&changes] { ++changes; });
    const Config::RemoteSettingsPatch batch{.keepAwake = std::optional(false),
                                            .finishSound = std::optional(false)};
    require(config.applyRemoteSettings(batch), "a valid settings batch applies");
    require(changes == 1, "one successful settings batch publishes once");
    const auto saved = config.remoteSettings();
    require(!saved.keepAwake && !saved.finishSound,
            "every value in a valid settings batch is applied");
    const auto written = read_config(path);
    require(!written.value(QStringLiteral("keepAwake")).toBool(true) &&
                !written.value(QStringLiteral("alerts"))
                     .toObject()
                     .value(QStringLiteral("finished"))
                     .toBool(true),
            "the whole settings batch is written once");
    require(written.value(QStringLiteral("retained")).toBool(),
            "settings persistence preserves unrelated config");

    const auto before_failure = config.remoteSettings();
    const QByteArray malformed = "{unfinished remote-settings edit";
    static_cast<void>(write_config(dir, malformed));
    config.setSourcePathForTesting(path); // Keep memory; watch the new bytes only.
    QString diagnostic;
    Config::RemoteSettingsPatch failed_batch;
    failed_batch.keepAwake = true;
    failed_batch.finishSound = true;
    require(!config.applyRemoteSettings(failed_batch, &diagnostic),
            "a failed settings save refuses the whole batch");
    require(config.remoteSettings().keepAwake == before_failure.keepAwake &&
                config.remoteSettings().finishSound == before_failure.finishSound,
            "a failed settings save rolls back every field it touched");
    require(diagnostic.contains(QStringLiteral("Could not save")),
            "a failed settings save reports its diagnostic");
    QFile raw(path);
    require(raw.open(QIODevice::ReadOnly), "read the failed settings config");
    require(raw.readAll() == malformed, "a failed settings save preserves disk bytes");
}

} // namespace

// Defaults for new agents, alerts and keeping awake come from the file, and
// an edit from anywhere (an agent rewriting it included) applies at once; the
// window's own saves are not read back as edits.
void config_reloads_when_edited_elsewhere() {
    QTemporaryDir directory;
    require(directory.isValid(), "temporary directory");
    const QDir dir(directory.path());
    const QString path =
        write_config(dir, R"({"version": 1, "theme": "graphite", "keepAwake": false,
                 "alerts": {"sound": false, "repeat": 5},
                 "newAgent": {"harness": "claude", "folder": "~/dev", "mode": "full",
                              "machines": {"devbox": {"folder": "~/work"}},
                              "models": {"codex": ["gpt-6-sol", "custom model [v2]"],
                                         "grok": ["-oBad", "grok-4.7"]}}})");
    Config config;
    config.setSourcePathForTesting(path);
    require(config.load(), "config with defaults should load");
    const auto& defaults = config.agentDefaults();
    require(defaults.harness == QStringLiteral("claude") &&
                defaults.folder == QStringLiteral("~/dev") &&
                defaults.machineFolders.value(QStringLiteral("devbox")) ==
                    QStringLiteral("~/work") &&
                defaults.mode == QStringLiteral("full"),
            "new-agent defaults are read");
    require(defaults.models.value(QStringLiteral("codex")) ==
                    QStringList{QStringLiteral("gpt-6-sol"), QStringLiteral("custom model [v2]")} &&
                defaults.models.value(QStringLiteral("grok")) ==
                    QStringList{QStringLiteral("grok-4.7")} &&
                !defaults.models.contains(QStringLiteral("claude")),
            "configured models replace what a CLI lists; names that look like options are dropped");
    // "sound" and "repeat" are the retired request chime's: ignored, not reported.
    require(config.finishSound() && !config.keepAwake() &&
                !config.diagnostic().contains(QStringLiteral("alerts")),
            "alerts and keeping awake are read, with defaults for what is missing");

    int changes = 0;
    QObject::connect(&config, &Config::changed, [&changes] { ++changes; });
    const auto settle = [](const std::function<bool()>& done, int milliseconds) {
        QElapsedTimer clock;
        clock.start();
        while (!done() && clock.elapsed() < milliseconds) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            QThread::msleep(10);
        }
        return done();
    };
    // Rewritten by replacement, as editors and agents usually write files.
    QSaveFile replacement(path);
    require(replacement.open(QIODevice::WriteOnly), "replace the config");
    replacement.write(R"({"version": 1, "theme": "amber"})");
    require(replacement.commit(), "commit the replacement");
    require(settle([&config] { return config.themeName() == QStringLiteral("amber"); }, 3000),
            "an edit elsewhere applies without a restart");
    require(config.agentDefaults().harness.isEmpty() && config.keepAwake(),
            "settings removed from the file return to their defaults");
    settle([] { return false; }, 400); // let the replacement's events pass
    changes = 0;
    require(config.setFinishSound(false) && config.setKeepAwake(false),
            "save alert and keep-awake choices");
    const int saved = changes;
    settle([] { return false; }, 600);
    require(changes == saved && !config.finishSound(), "the window's own save is not read back");
    const QJsonObject written = read_config(path);
    require(!written.value(QStringLiteral("alerts"))
                    .toObject()
                    .value(QStringLiteral("finished"))
                    .toBool(true) &&
                !written.value(QStringLiteral("keepAwake")).toBool(true) &&
                !written.contains(QStringLiteral("usage")) &&
                written.value(QStringLiteral("theme")).toString() == QStringLiteral("amber"),
            "alert choices are written beside the rest of the config");
}

// A window built without a config owns a defaults-only one. It never reads,
// watches or writes a file, even the one LAPIS_CONFIG names, and its changes
// stay in memory.
void defaults_only_touches_no_file() {
    QTemporaryDir directory;
    require(directory.isValid(), "temporary directory");
    const QDir dir(directory.path());
    const QByteArray contents = R"({"version": 1, "theme": "amber", "previewsVisible": true})";
    const QString path = write_config(dir, contents);
    const QByteArray previous = qgetenv("LAPIS_CONFIG");
    const bool had_previous = qEnvironmentVariableIsSet("LAPIS_CONFIG");
    qputenv("LAPIS_CONFIG", QFile::encodeName(path));
    const auto restore = qScopeGuard([&] {
        if (had_previous)
            qputenv("LAPIS_CONFIG", previous);
        else
            qunsetenv("LAPIS_CONFIG");
    });
    {
        Config config{Config::DefaultsOnly{}};
        require(!config.fileBacked() && config.sourcePath().isEmpty(),
                "a defaults-only config names no file");
        require(config.themeName() == QStringLiteral("lapis") && !config.previewsVisible() &&
                    config.diagnostic().isEmpty(),
                "a defaults-only config starts from the built-in defaults, not the file");
        require(config.sequences(QStringLiteral("openSettings")) == default_settings_shortcuts(),
                "a defaults-only config has the default bindings");
        require(config.setTheme(QStringLiteral("graphite")) &&
                    config.themeName() == QStringLiteral("graphite"),
                "a change applies in memory");
        require(config.setPreviewsVisible(true) && config.previewsVisible() &&
                    config.diagnostic().isEmpty(),
                "a change reports no save failure");
        // An edit to the file is not watched.
        QSaveFile replacement(path);
        require(replacement.open(QIODevice::WriteOnly), "replace the config");
        replacement.write(R"({"version": 1, "theme": "oled"})");
        require(replacement.commit(), "commit the replacement");
        QElapsedTimer clock;
        clock.start();
        while (clock.elapsed() < 400) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            QThread::msleep(10);
        }
        require(config.themeName() == QStringLiteral("graphite"), "the file is not watched");
        require(!config.reload() && config.themeName() == QStringLiteral("lapis") &&
                    config.diagnostic().isEmpty(),
                "a reload returns to the defaults without reading the file");
        require(read_config(path).value(QStringLiteral("theme")).toString() ==
                        QStringLiteral("oled") &&
                    dir.entryList(QDir::Files | QDir::Hidden) ==
                        QStringList{QStringLiteral("lapis.json")},
                "nothing is written beside the named file");
    }
}

// Settings of retired features load without a diagnostic, and saving another
// setting leaves them exactly as written.
void retired_plan_settings_survive() {
    QTemporaryDir directory;
    require(directory.isValid(), "temporary directory");
    const QString path =
        write_config(QDir(directory.path()),
                     R"({"version": 1, "limitResets": {"auto": true, "minBlockedMinutes": 30},
            "accounts": {"switchAt": 90,
                         "claude": [{"name": "mine", "email": "me@example.com", "home": "local"},
                                    {"name": "spare", "machines": ["local", "devbox"]}],
                         "codex": [{"name": "work", "email": "work@example.com"}]},
            "usage": {"show": false, "meter": ["codex"], "machines": ["devbox"]},
            "showUsage": true})");
    const auto before = read_config(path);
    Config config;
    config.setSourcePathForTesting(path);
    require(config.load() && config.diagnostic().isEmpty(),
            "retired settings load without a diagnostic");
    require(config.setFinishSound(false), "another setting saves");
    const auto after = read_config(path);
    for (const auto* key : {"limitResets", "accounts", "usage", "showUsage"})
        require(after.value(QLatin1String(key)) == before.value(QLatin1String(key)),
                "saving leaves a retired setting as written");
}

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    try {
        themes_are_complete();
        oled_theme_is_black();
        sidebar_preference_and_commands();
        typing_scroll_preference_persists();
        visual_settings_persist();
        visual_keep_budget_parses();
        appearance_switches_kinds();
        settings_write_known_keys();
        selection_persists();
        symbolic_link_config_updates_target();
        appearance_preserves_json_and_bad_files();
        deeply_nested_values_survive_persistence();
        empty_objects_are_compact();
        expanded_config_remains_reloadable();
        zero_byte_file_initializes();
        nonregular_config_paths_are_rejected();
        default_settings_chords_are_shared();
        navigation_defaults_preserve_terminal_editing();
        retired_actions_load_silently();
        configured_shortcuts_are_validated();
        unknown_names_are_rejected();
        malformed_values_fall_back();
        harness_arguments_are_literal_lists();
        harness_updates_can_be_turned_off();
        advertised_names_are_accepted();
        terminal_font_persists_and_rolls_back();
        remote_settings_are_transactional();
        config_reloads_when_edited_elsewhere();
        retired_plan_settings_survive();
        defaults_only_touches_no_file();
    } catch (const std::exception& error) {
        std::cerr << "config_test: " << error.what() << '\n';
        return 1;
    }
    std::cout << "config_test: all cases passed\n";
    return 0;
}
