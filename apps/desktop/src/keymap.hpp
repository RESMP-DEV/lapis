#ifndef LAPIS_DESKTOP_KEYMAP_HPP
#define LAPIS_DESKTOP_KEYMAP_HPP

#include <QFileSystemWatcher>
#include <QHash>
#include <QJsonValue>
#include <QKeyCombination>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

#include <array>

#include <cstdint>
#include <optional>

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
// the program's output. Each semantic colour has one meaning: focused_border
// marks where keyboard input goes, activity marks a working agent, attention
// marks pending requests only, and fault marks lost connections and errors.
struct Theme {
    const char* name{};
    const char* label{};
    const char* background{};
    const char* surface{};
    const char* card{};
    const char* hovered_card{};
    const char* focused{};
    const char* border{};
    const char* focused_border{};
    const char* text{};
    const char* muted_text{};
    const char* attention{};
    const char* attention_text{};
    const char* activity{};
    const char* fault{};
    // A usage gauge: plenty left (green) and nearly none (red).
    const char* plenty{};
    const char* scarce{};
    int corner_radius{2};
    int motion_ms{100};
    bool mono_chrome{};
    double heading_tracking{1.0};
};

// Built-in colour schemes, indexed by ThemeName. Kept in one table so the
// settings dialog, the QML tokens, and the config file all agree.
[[nodiscard]] const Theme& theme_for(const QString& name);
[[nodiscard]] const std::array<Theme, 7>& theme_table();
[[nodiscard]] bool theme_exists(const QString& name);
[[nodiscard]] QStringList default_settings_shortcuts();
// X11 reports Shift with punctuation as the shifted symbol: Ctrl+Shift+,
// arrives as Ctrl+Shift+<. This is that form for US-layout punctuation.
[[nodiscard]] std::optional<QKeyCombination> shifted_punctuation(QKeyCombination combination);

// Terminal text size in pixels. Machine readouts in the chrome share the same
// family at the chrome's own size.
inline constexpr int kTerminalFontSizeDefault = 16;
inline constexpr int kTerminalFontSizeMinimum = 10;
inline constexpr int kTerminalFontSizeMaximum = 32;

// Defaults for new agents, from the config's "newAgent" section: the CLI,
// the folder to start in on this Mac and on each ssh machine, and the models
// offered per CLI (the CLI's own default is always offered too).
struct AgentDefaults {
    QString harness;
    QString folder;
    QString mode; // edits, auto or full; empty until the config names one
    QHash<QString, QString> machineFolders;
    // Names a CLI offers instead of the models it lists itself.
    QHash<QString, QStringList> models;
};

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
    Q_PROPERTY(bool sidebarVisible READ sidebarVisible NOTIFY changed)
    // The strip of live agent previews under the stage.
    Q_PROPERTY(bool previewsVisible READ previewsVisible NOTIFY changed)
    Q_PROPERTY(QString diagnostic READ diagnostic NOTIFY changed)
    Q_PROPERTY(QString sourcePath READ sourcePath NOTIFY changed)
    Q_PROPERTY(QVariantMap shortcutBindings READ shortcutBindings NOTIFY changed)
    // Empty means the platform's fixed-width system font. Availability is
    // resolved by the GUI surface; the keymap only validates and persists.
    Q_PROPERTY(QString terminalFontFamily READ terminalFontFamily NOTIFY changed)
    Q_PROPERTY(int terminalFontSize READ terminalFontSize NOTIFY changed)
    Q_PROPERTY(int terminalFontSizeMinimum READ terminalFontSizeMinimum CONSTANT)
    Q_PROPERTY(int terminalFontSizeMaximum READ terminalFontSizeMaximum CONSTANT)
    Q_PROPERTY(int terminalFontSizeDefault READ terminalFontSizeDefault CONSTANT)
    // A chime when an agent needs you, repeated while it waits unseen, and
    // another when a Codex or Claude turn ends out of view.
    Q_PROPERTY(bool alertSound READ alertSound NOTIFY changed)
    Q_PROPERTY(bool finishSound READ finishSound NOTIFY changed)
    Q_PROPERTY(int alertRepeat READ alertRepeat NOTIFY changed)
    // Keep this Mac from sleeping on power, so the phone can reach it.
    Q_PROPERTY(bool keepAwake READ keepAwake NOTIFY changed)
    // Plan usage under the categories, and the dashboard's machines.
    Q_PROPERTY(bool showUsage READ showUsage NOTIFY changed)
    Q_PROPERTY(QStringList usageMeter READ usageMeter NOTIFY changed)
    Q_PROPERTY(QStringList usageMachines READ usageMachines NOTIFY changed)
  public:
    explicit KeyMap(QObject* parent = nullptr);

    // Re-read the file, keeping defaults for anything missing or invalid.
    // Returns true when the file parsed and was applied.
    bool load();
    [[nodiscard]] bool loaded() const { return loaded_; }
    [[nodiscard]] const QString& sourcePath() const { return source_path_; }
    // Point the config at another file. Used by tests so they never touch the
    // user's real lapis.json; not part of the QML surface.
    void setSourcePathForTesting(const QString& path) {
        source_path_ = path;
        watch();
    }
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
    Q_INVOKABLE bool setSidebarVisible(bool visible);
    [[nodiscard]] bool sidebarVisible() const { return sidebar_visible_; }
    Q_INVOKABLE bool setPreviewsVisible(bool visible);
    [[nodiscard]] bool previewsVisible() const { return previews_visible_; }
    // Terminal font changes apply live and are written immediately. A value
    // that cannot be saved is rolled back so the window matches the file.
    Q_INVOKABLE bool setTerminalFontFamily(const QString& family);
    Q_INVOKABLE bool setTerminalFontSize(int pixels);
    [[nodiscard]] const QString& terminalFontFamily() const { return terminal_font_family_; }
    [[nodiscard]] int terminalFontSize() const { return terminal_font_size_; }
    // Literal arguments added when lapis starts an agent of each harness, for
    // example {"claude": ["--dangerously-skip-permissions"]}. Explicit user
    // configuration; lapis adds no execution or approval flags itself.
    [[nodiscard]] const QHash<QString, QStringList>& harnessArguments() const {
        return harness_arguments_;
    }
    Q_INVOKABLE bool setAlertSound(bool on);
    Q_INVOKABLE bool setFinishSound(bool on);
    Q_INVOKABLE bool setAlertRepeat(int times);
    Q_INVOKABLE bool setKeepAwake(bool on);
    Q_INVOKABLE bool setShowUsage(bool on);
    [[nodiscard]] bool alertSound() const { return alert_sound_; }
    [[nodiscard]] bool finishSound() const { return finish_sound_; }
    [[nodiscard]] int alertRepeat() const { return alert_repeat_; }
    [[nodiscard]] bool keepAwake() const { return keep_awake_; }
    [[nodiscard]] bool showUsage() const { return show_usage_; }
    // The meter's plans in order (empty: every signed-in plan), and the ssh
    // hosts with a usage dashboard beside this Mac.
    [[nodiscard]] const QStringList& usageMeter() const { return usage_meter_; }
    [[nodiscard]] const QStringList& usageMachines() const { return usage_machines_; }
    [[nodiscard]] const AgentDefaults& agentDefaults() const { return agent_defaults_; }
    [[nodiscard]] static int terminalFontSizeMinimum() { return kTerminalFontSizeMinimum; }
    [[nodiscard]] static int terminalFontSizeMaximum() { return kTerminalFontSizeMaximum; }
    [[nodiscard]] static int terminalFontSizeDefault() { return kTerminalFontSizeDefault; }
    // Write the current appearance back to the config file. Returns false and
    // sets diagnostic() when the file cannot be written, leaving the in-memory
    // choice active so the running window still matches what was selected.
    Q_INVOKABLE bool save();

  signals:
    void changed();

  private:
    void apply_defaults();
    void load_terminal_font(const QJsonValue& value);
    void load_harness_arguments(const QJsonValue& value);
    void load_alerts(const QJsonObject& root);
    void load_usage(const QJsonObject& root);
    void load_agent_defaults(const QJsonValue& value);
    // The file is watched, so an edit from anywhere (an agent included)
    // applies at once; the window's own saves are recognised and skipped.
    void watch();
    void fileTouched();
    [[nodiscard]] static QString default_source_path();
    // Rewrite only the appearance keys, preserving keybindings and categories
    // as they appear on disk. Returns false when the file was not written.
    [[nodiscard]] bool persist();
    [[nodiscard]] bool save_without_tentative_change();
    QHash<QString, QStringList> bindings_;
    QString source_path_;
    QString diagnostic_;
    WorkspaceLayout layout_{WorkspaceLayout::Focus};
    CardDensity density_{CardDensity::Comfortable};
    QString theme_{QStringLiteral("lapis")};
    QString terminal_font_family_;
    int terminal_font_size_{kTerminalFontSizeDefault};
    QHash<QString, QStringList> harness_arguments_;
    bool loaded_{};
    bool sidebar_visible_{true};
    bool previews_visible_{true};
    bool alert_sound_{true};
    bool finish_sound_{true};
    int alert_repeat_{3};
    bool keep_awake_{true};
    bool show_usage_{true};
    QStringList usage_meter_;
    QStringList usage_machines_;
    AgentDefaults agent_defaults_;
    QFileSystemWatcher watcher_;
    QTimer settle_;
    QByteArray known_contents_;
};
} // namespace lapis::desktop
#endif // LAPIS_DESKTOP_KEYMAP_HPP
