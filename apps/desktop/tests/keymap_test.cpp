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
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <iostream>
#include <stdexcept>

namespace {
using lapis::desktop::CardDensity;
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
        appearance_preserves_json_and_bad_files();
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