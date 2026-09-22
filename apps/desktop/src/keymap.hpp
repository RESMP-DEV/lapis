#ifndef LAPIS_DESKTOP_KEYMAP_HPP
#define LAPIS_DESKTOP_KEYMAP_HPP

#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

#include <array>

#include <cstdint>

namespace lapis::desktop {
// Layout of the session area. "focus" keeps one large pane with a strip of
// cards; "blocks" gives every session an equal tile; "columns" stacks the
// previews in a left channel beside the pane; "stack" shows one session at a
// time with the carousel hidden.
enum class WorkspaceLayout : std::uint8_t { Focus, Blocks, Columns, Stack };

// How much room the preview cards take in the focus and columns layouts.
enum class CardDensity : std::uint8_t { Comfortable, Compact, Minimal };

// Colour scheme for the window chrome. Terminal cell colours come from the
// session's own palette, so a theme changes the frame around the terminal, not
// the program's output.
struct Theme {
    const char* name;
    const char* label;
    const char* background;
    const char* surface;
    const char* card;
    const char* hovered_card;
    const char* focused;
    const char* border;
    const char* focused_border;
    const char* text;
    const char* muted_text;
    const char* attention;
    const char* attention_text;
};

// Built-in colour schemes, indexed by ThemeName. Kept in one table so the
// settings dialog, the QML tokens, and the config file all agree.
[[nodiscard]] const Theme& theme_for(const QString& name);
[[nodiscard]] const std::array<Theme, 6>& theme_table();
[[nodiscard]] bool theme_exists(const QString& name);
[[nodiscard]] QStringList default_settings_shortcuts();

// User-editable keybindings, layout, theme, and card density, loaded from
// lapis.json at the project root. Missing or malformed input falls back to
// built-in defaults, so a bad edit degrades rather than bricking the window.
// Every action maps to a list of Qt key sequences, which QML binds through
// Shortcut.
class KeyMap final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString layoutName READ layoutName NOTIFY changed)
    Q_PROPERTY(bool blocks READ blocks NOTIFY changed)
    Q_PROPERTY(QString themeName READ themeName NOTIFY changed)
    Q_PROPERTY(QString densityName READ densityName NOTIFY changed)
    Q_PROPERTY(QVariantList themes READ themes NOTIFY changed)
    Q_PROPERTY(QStringList layouts READ layouts NOTIFY changed)
    Q_PROPERTY(QStringList densities READ densities NOTIFY changed)
    Q_PROPERTY(QString diagnostic READ diagnostic NOTIFY changed)
    Q_PROPERTY(QString sourcePath READ sourcePath NOTIFY changed)
    Q_PROPERTY(QVariantMap shortcutBindings READ shortcutBindings NOTIFY changed)
  public:
    explicit KeyMap(QObject* parent = nullptr);

    // Re-read the file, keeping defaults for anything missing or invalid.
    // Returns true when the file parsed and was applied.
    bool load();
    [[nodiscard]] bool loaded() const { return loaded_; }
    [[nodiscard]] const QString& sourcePath() const { return source_path_; }
    // Point the config at another file. Used by tests so they never touch the
    // user's real lapis.json; not part of the QML surface.
    void setSourcePathForTesting(const QString& path) { source_path_ = path; }
    [[nodiscard]] const QString& diagnostic() const { return diagnostic_; }

    [[nodiscard]] QStringList sequences(const QString& action) const;
    [[nodiscard]] QVariantMap shortcutBindings() const;
    Q_INVOKABLE [[nodiscard]] QStringList actionSequences(const QString& action) const {
        return sequences(action);
    }

    [[nodiscard]] WorkspaceLayout layout() const { return layout_; }
    [[nodiscard]] QString layoutName() const;
    [[nodiscard]] bool blocks() const { return layout_ == WorkspaceLayout::Blocks; }
    [[nodiscard]] const QString& themeName() const { return theme_; }
    [[nodiscard]] QString densityName() const;
    [[nodiscard]] QVariantList themes() const;
    [[nodiscard]] QStringList layouts() const;
    [[nodiscard]] QStringList densities() const;

    // Cycle through layouts() in order and persist through setLayout().
    Q_INVOKABLE void toggleLayout();
    Q_INVOKABLE bool reload();

    // Apply an appearance change from the settings dialog. Each writes the
    // config file so the choice survives a restart, then moves keyboard focus
    // back to the terminal. Invalid names are rejected without touching state.
    Q_INVOKABLE bool setLayout(const QString& name);
    Q_INVOKABLE bool setTheme(const QString& name);
    Q_INVOKABLE bool setDensity(const QString& name);
    // Write the current appearance back to the config file. Returns false and
    // sets diagnostic() when the file cannot be written, leaving the in-memory
    // choice active so the running window still matches what was selected.
    Q_INVOKABLE bool save();

  signals:
    void changed();

  private:
    void apply_defaults();
    [[nodiscard]] static QString default_source_path();
    // Rewrite only the appearance keys, preserving keybindings and categories
    // as they appear on disk. Returns false when the file was not written.
    [[nodiscard]] bool persist();
    QHash<QString, QStringList> bindings_;
    QString source_path_;
    QString diagnostic_;
    WorkspaceLayout layout_{WorkspaceLayout::Focus};
    CardDensity density_{CardDensity::Comfortable};
    QString theme_{QStringLiteral("lapis")};
    bool loaded_{};
};
} // namespace lapis::desktop
#endif // LAPIS_DESKTOP_KEYMAP_HPP
