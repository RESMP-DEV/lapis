// Appearance configuration behavior: theme, layout, and density selection,
// validation of unknown names, and persistence back to the config file.
//
// The keymap reads a real file rather than a stub, so these cases exercise the
// same path the settings dialog uses, including the write-back that makes a
// choice survive a restart.
#include "keymap.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QTemporaryDir>
#include <iostream>
#include <stdexcept>

namespace {
using lapis::desktop::CardDensity;
using lapis::desktop::default_settings_shortcuts;
using lapis::desktop::KeyMap;
using lapis::desktop::theme_table;
using lapis::desktop::WorkspaceLayout;

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
        const char* fields[] = {theme.background, theme.surface,       theme.card,
                                theme.border,     theme.text,          theme.muted_text,
                                theme.attention,  theme.focused_border};
        for (const char* value : fields)
            require(value != nullptr && *value == '#', "theme colour must be a hex literal");
        // Names are the config keys, so a duplicate would make one unreachable.
        for (std::size_t j = i + 1; j < themes.size(); ++j)
            require(QString::fromLatin1(themes[i].name) != QString::fromLatin1(themes[j].name),
                    "theme names must be unique");
    }
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

    KeyMap keymap;
    require(keymap.load(), "default config should load");
    keymap.setSourcePathForTesting(path);
    require(keymap.load(), "explicit config should load");

    require(keymap.setTheme(QStringLiteral("graphite")), "graphite theme should apply");
    require(keymap.themeName() == QStringLiteral("graphite"), "theme should be remembered");
    require(keymap.setLayout(QStringLiteral("columns")), "columns layout should apply");
    require(keymap.layoutName() == QStringLiteral("columns"), "layout should be remembered");
    require(keymap.setDensity(QStringLiteral("compact")), "compact density should apply");
    require(keymap.densityName() == QStringLiteral("compact"), "density should be remembered");

    const QJsonObject written = read_config(path);
    require(written.value(QStringLiteral("theme")).toString() == QStringLiteral("graphite"),
            "theme should be written to the file");
    require(written.value(QStringLiteral("layout")).toString() == QStringLiteral("columns"),
            "layout should be written to the file");
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
    KeyMap reloaded;
    reloaded.setSourcePathForTesting(path);
    require(reloaded.load(), "written config should reload");
    require(reloaded.themeName() == QStringLiteral("graphite"), "theme should survive a reload");
    require(reloaded.layoutName() == QStringLiteral("columns"), "layout should survive a reload");
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
    KeyMap keymap;
    keymap.setSourcePathForTesting(chain);
    require(keymap.load(), "linked config should load");
    require(keymap.setTheme(QStringLiteral("graphite")), "linked config should save");
    require(QFileInfo(link).isSymbolicLink() && QFileInfo(chain).isSymbolicLink(),
            "appearance save replaced a config symlink");
    require(QFileInfo(chain).canonicalFilePath() == canonical_target, "config link target changed");
    const auto saved = read_config(target);
    require(saved.value(QStringLiteral("theme")).toString() == QStringLiteral("graphite") &&
                saved.value(QStringLiteral("retained")).toBool(),
            "linked save did not preserve and update the actual target");
    require(keymap.load() && keymap.themeName() == QStringLiteral("graphite"),
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
    KeyMap keymap;
    keymap.setSourcePathForTesting(path);
    require(keymap.load(), "load punctuation fixture");
    require(keymap.setTheme(QStringLiteral("graphite")), "persist punctuation fixture");
    const QJsonObject after = read_config(path);
    require(after.value(QStringLiteral("keybindings")) ==
                before.value(QStringLiteral("keybindings")),
            "appearance save changed a shortcut");
    require(after.value(QStringLiteral("categories")) == before.value(QStringLiteral("categories")),
            "appearance save changed category text");
    const QByteArray malformed = "{unfinished user edit";
    static_cast<void>(write_config(dir, malformed));
    require(!keymap.setDensity(QStringLiteral("compact")), "refuse to overwrite invalid JSON");
    QFile raw(path);
    require(raw.open(QIODevice::ReadOnly), "read invalid config");
    require(raw.readAll() == malformed, "invalid config must be preserved exactly");
    require(!keymap.diagnostic().isEmpty(), "failed save is visible");
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

    KeyMap keymap;
    keymap.setSourcePathForTesting(path);
    require(keymap.load(), "768-level JSON is accepted by Qt and loaded");
    require(keymap.setTheme(QStringLiteral("graphite")), "deep config should save");

    const QJsonObject written = read_config(path);
    require(written.value(QStringLiteral("nested")) == nested,
            "deep nested values should be preserved");
    KeyMap reloaded;
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

    KeyMap keymap;
    keymap.setSourcePathForTesting(path);
    require(keymap.load(), "empty-object fixture should load");
    require(keymap.setDensity(QStringLiteral("minimal")), "empty-object fixture should save");

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
    KeyMap keymap;
    keymap.setSourcePathForTesting(path);
    require(keymap.load(), "compact array fixture loads");
    require(keymap.setTheme(QStringLiteral("amber")), "large config saves within read cap");
    require(keymap.reload(), "saved large config reloads");
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

    KeyMap keymap;
    keymap.setSourcePathForTesting(path);
    require(!keymap.load(), "zero-byte file has no JSON object yet");
    require(keymap.setTheme(QStringLiteral("daylight")), "zero-byte file should initialize");
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

    KeyMap keymap;
    keymap.setSourcePathForTesting(path);
    require(!keymap.load(), "load must reject a nonregular path");
    require(!keymap.save(), "persist must reject a nonregular path");
}

// The dialog must expose both platform spellings without changing custom
// sequences loaded from the user's config.
void default_settings_chords_are_shared() {
    const QStringList expected{QStringLiteral("Ctrl+,"), QStringLiteral("Meta+,")};
    require(default_settings_shortcuts() == expected,
            "exported settings defaults must contain both chords");
    KeyMap keymap;
    require(keymap.actionSequences(QStringLiteral("openSettings")) == expected,
            "KeyMap defaults must use the exported settings chords");

    QTemporaryDir directory;
    require(directory.isValid(), "temporary directory for custom binding");
    const QDir dir(directory.path());
    const QString path =
        write_config(dir, QByteArrayLiteral(R"({"keybindings":{"openSettings":[" Alt+F4 "]}})"));
    keymap.setSourcePathForTesting(path);
    require(keymap.load(), "custom settings fixture should load");
    const QStringList custom = keymap.actionSequences(QStringLiteral("openSettings"));
    require(custom == QStringList{QStringLiteral("Alt+F4")},
            "custom bindings must replace defaults exactly, with only surrounding space trimmed");
}

// Unknown names are rejected without disturbing the current selection, so a bad
// config edit or a stale dialog cannot leave the window in an undefined state.
void unknown_names_are_rejected() {
    QTemporaryDir directory;
    require(directory.isValid(), "temporary directory");
    const QDir dir(directory.path());
    const QString path = write_config(dir, R"({"version": 1})");

    KeyMap keymap;
    keymap.setSourcePathForTesting(path);
    require(keymap.load(), "config should load");
    const QString theme = keymap.themeName();
    const QString layout = keymap.layoutName();
    const QString density = keymap.densityName();

    require(!keymap.setTheme(QStringLiteral("chartreuse")), "unknown theme must be rejected");
    require(!keymap.setLayout(QStringLiteral("spiral")), "unknown layout must be rejected");
    require(!keymap.setDensity(QStringLiteral("enormous")), "unknown density must be rejected");
    require(keymap.themeName() == theme, "a rejected theme must not change the selection");
    require(keymap.layoutName() == layout, "a rejected layout must not change the selection");
    require(keymap.densityName() == density, "a rejected density must not change the selection");
    require(!keymap.diagnostic().isEmpty(), "a rejected name should explain itself");
}

// The loader tolerates a malformed file by keeping defaults, which is what makes
// a hand-edited config safe to ship.
void malformed_values_fall_back() {
    QTemporaryDir directory;
    require(directory.isValid(), "temporary directory");
    const QDir dir(directory.path());
    const QString path =
        write_config(dir, R"({"layout": "diagonal", "theme": "vaporwave", "density": "gigantic"})");

    KeyMap keymap;
    keymap.setSourcePathForTesting(path);
    require(keymap.load(), "a syntactically valid file should still load");
    require(keymap.layoutName() == QStringLiteral("focus"), "unknown layout should fall back");
    require(keymap.themeName() == QStringLiteral("lapis"), "unknown theme should fall back");
    require(keymap.densityName() == QStringLiteral("comfortable"),
            "unknown density should fall back");
    require(!keymap.diagnostic().isEmpty(), "falling back should be reported");
}

// The dialog builds its controls from these lists, so they must agree with the
// names the setters accept.
void advertised_names_are_accepted() {
    QTemporaryDir directory;
    require(directory.isValid(), "temporary directory");
    const QDir dir(directory.path());
    const QString path = write_config(dir, R"({"version": 1})");

    KeyMap keymap;
    keymap.setSourcePathForTesting(path);
    require(keymap.load(), "config should load");

    for (const QString& name : keymap.layouts())
        require(keymap.setLayout(name), "every advertised layout must be accepted");
    for (const QString& name : keymap.densities())
        require(keymap.setDensity(name), "every advertised density must be accepted");
    const QVariantList themes = keymap.themes();
    require(!themes.isEmpty(), "the dialog needs at least one theme");
    for (const QVariant& entry : themes) {
        const QString name = entry.toMap().value(QStringLiteral("name")).toString();
        require(!name.isEmpty(), "each advertised theme needs a name");
        require(keymap.setTheme(name), "every advertised theme must be accepted");
    }
    // The final iteration leaves the last advertised name selected, and the
    // enumeration agrees with the string names written to the file.
    require(keymap.layout() == WorkspaceLayout::Stack, "last advertised layout is stack");
    require(keymap.densityName() == QStringLiteral("minimal"),
            "last advertised density is minimal");
    require(static_cast<int>(CardDensity::Comfortable) == 0, "density order is part of the file");
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    try {
        themes_are_complete();
        selection_persists();
        symbolic_link_config_updates_target();
        appearance_preserves_json_and_bad_files();
        deeply_nested_values_survive_persistence();
        empty_objects_are_compact();
        expanded_config_remains_reloadable();
        zero_byte_file_initializes();
        nonregular_config_paths_are_rejected();
        default_settings_chords_are_shared();
        unknown_names_are_rejected();
        malformed_values_fall_back();
        advertised_names_are_accepted();
    } catch (const std::exception& error) {
        std::cerr << "keymap_test: " << error.what() << '\n';
        return 1;
    }
    std::cout << "keymap_test: all cases passed\n";
    return 0;
}
