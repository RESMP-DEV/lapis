#ifndef LAPIS_DESKTOP_CONFIG_HPP
#define LAPIS_DESKTOP_CONFIG_HPP

#include "interaction_log.hpp"
#include "next_prompt.hpp"
#include "tab_ranker.hpp"
#include <QFileSystemWatcher>
#include <QHash>
#include <QJsonObject>
#include <QJsonValue>
#include <QKeyCombination>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

#include <array>

#include <cstdint>
#include <functional>
#include <optional>

namespace lapis::desktop {
// How much room the preview cards take in the strip under the stage.
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
    // A healthy state (green), such as a CLI the Tools pane found working.
    const char* plenty{};
    int corner_radius{2};
    int motion_ms{100};
    // Dark text on a light surface: the window theme light mode uses, and
    // the one that takes the darker category colours.
    bool light{};
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
inline constexpr int kTerminalFontSizeDefault = 14;
inline constexpr int kTerminalFontSizeMinimum = 10;
inline constexpr int kTerminalFontSizeMaximum = 32;

// Defaults for new agents, from the config's "newAgent" section: the CLI,
// the folder to start in on this Mac and on each ssh machine, and the models
// offered per CLI; a configured list replaces the CLI's own list.
struct AgentDefaults {
    QString harness;
    QString folder;
    QString mode; // edits, auto or full; empty until the config names one
    QHash<QString, QString> machineFolders;
    // Names a CLI offers instead of the models it lists itself.
    QHash<QString, QStringList> models;
};

// User-editable keybindings, theme, and card density, loaded from
// lapis.json at the project root. Missing or malformed input falls back to
// built-in defaults, so a bad edit degrades rather than bricking the window.
// Every action maps to a list of Qt key sequences, which QML binds through
// Shortcut.
class Config final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString themeName READ themeName NOTIFY changed)
    Q_PROPERTY(QString densityName READ densityName NOTIFY changed)
    Q_PROPERTY(QVariantList themes READ themes NOTIFY changed)
    Q_PROPERTY(QStringList densities READ densities NOTIFY changed)
    Q_PROPERTY(bool sidebarVisible READ sidebarVisible NOTIFY changed)
    // The strip of live agent previews under the stage.
    Q_PROPERTY(bool previewsVisible READ previewsVisible NOTIFY changed)
    Q_PROPERTY(QString diagnostic READ diagnostic NOTIFY diagnosticChanged)
    Q_PROPERTY(QString sourcePath READ sourcePath NOTIFY changed)
    Q_PROPERTY(QVariantMap shortcutBindings READ shortcutBindings NOTIFY changed)
    // Empty means the platform's fixed-width system font. Availability is
    // resolved by the GUI surface; the config only validates and persists.
    Q_PROPERTY(QString terminalFontFamily READ terminalFontFamily NOTIFY changed)
    Q_PROPERTY(int terminalFontSize READ terminalFontSize NOTIFY changed)
    Q_PROPERTY(int terminalFontSizeMinimum READ terminalFontSizeMinimum CONSTANT)
    Q_PROPERTY(int terminalFontSizeMaximum READ terminalFontSizeMaximum CONSTANT)
    Q_PROPERTY(int terminalFontSizeDefault READ terminalFontSizeDefault CONSTANT)
    // A chime when a Codex or Claude turn ends out of view, open requests
    // included.
    Q_PROPERTY(bool finishSound READ finishSound NOTIFY changed)
    // Keep this Mac from sleeping on power, so the phone can reach it.
    Q_PROPERTY(bool keepAwake READ keepAwake NOTIFY changed)
    // Typing over a scrolled-back full-screen program returns it to its
    // bottom before the key is delivered.
    Q_PROPERTY(bool scrollToBottomOnTyping READ scrollToBottomOnTyping NOTIFY changed)
    // The visual view (a conversation as a thread instead of a terminal):
    // its color theme and reading and code fonts by name, its text size in
    // pixels (0 follows the terminal's), and whether every agent shows it.
    Q_PROPERTY(QVariantMap visual READ visual NOTIFY changed)
    // False for a defaults-only config: it has no file to reload.
    Q_PROPERTY(bool fileBacked READ fileBacked CONSTANT)
    // The settings Settings changes through setSetting, by dotted path:
    // newAgent.harness, newAgent.folder, newAgent.mode, nextPrompt.auto,
    // interactionLog.enabled, tabAway.overdueHours, and harnessUpdatesOff (the
    // CLIs not updated automatically).
    Q_PROPERTY(QVariantMap settings READ settings NOTIFY changed)
  public:
    explicit Config(QObject* parent = nullptr);
    // Built-in defaults that read, watch and write no file. Changes apply in
    // memory only. A window given no config uses one, so QML always has one.
    struct DefaultsOnly {};
    explicit Config(DefaultsOnly, QObject* parent = nullptr);
    [[nodiscard]] bool fileBacked() const { return file_backed_; }

    // Re-read the file, keeping defaults for anything missing or invalid.
    // Returns true when the file parsed and was applied.
    bool load();
    [[nodiscard]] bool loaded() const { return loaded_; }
    [[nodiscard]] const QString& sourcePath() const { return source_path_; }
    // Point the config at another file. Used by tests so they never touch the
    // user's real lapis.json; not part of the QML surface.
    void setSourcePathForTesting(const QString& path) {
        if (!file_backed_)
            return;
        source_path_ = path;
        watch();
    }
    [[nodiscard]] QString diagnostic() const {
        if (chime_diagnostic_.isEmpty())
            return diagnostic_;
        return diagnostic_.isEmpty() ? chime_diagnostic_
                                     : diagnostic_ + QLatin1Char('\n') + chime_diagnostic_;
    }
    // GUI-thread runtime status, independent of configuration-change signals.
    void setChimeDiagnostic(const QString& diagnostic);

    [[nodiscard]] QStringList sequences(const QString& action) const;
    [[nodiscard]] QVariantMap shortcutBindings() const;
    Q_INVOKABLE [[nodiscard]] QStringList actionSequences(const QString& action) const {
        return sequences(action);
    }

    [[nodiscard]] const QString& themeName() const { return theme_; }
    [[nodiscard]] QString densityName() const;
    [[nodiscard]] QVariantList themes() const;
    [[nodiscard]] QStringList densities() const;

    Q_INVOKABLE bool reload();

    // Apply an appearance change from the settings dialog. Each writes the
    // config file so the choice survives a restart, then moves keyboard focus
    // back to the terminal. Invalid names are rejected without touching state.
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
    // CLIs whose automatic updates lapis skips, from
    // {"harnessUpdates": {"omp": false}}; every other CLI updates as before.
    [[nodiscard]] const QSet<QString>& harnessUpdatesOff() const { return harness_updates_off_; }
    Q_INVOKABLE bool setFinishSound(bool on);
    Q_INVOKABLE bool setKeepAwake(bool on);
    Q_INVOKABLE bool setScrollToBottomOnTyping(bool on);
    [[nodiscard]] bool scrollToBottomOnTyping() const { return scroll_to_bottom_on_typing_; }
    // The values remote control may read. Config owns their storage and their
    // relationship to lapis.json.
    struct RemoteSettings {
        bool keepAwake{};
        bool finishSound{};
        [[nodiscard]] bool operator==(const RemoteSettings&) const = default;
    };
    // Absent fields leave their settings alone. Present fields are applied
    // together or not at all: one save writes the batch, and a failed save
    // restores the changed fields without writing the file.
    struct RemoteSettingsPatch {
        std::optional<bool> keepAwake;
        std::optional<bool> finishSound;
    };
    [[nodiscard]] RemoteSettings remoteSettings() const;
    [[nodiscard]] bool applyRemoteSettings(const RemoteSettingsPatch& changes,
                                           QString* diagnostic = nullptr);
    [[nodiscard]] bool finishSound() const { return finish_sound_; }
    // Sound files in place of the synthesized chimes (alerts.soundFile and
    // alerts.finishedFile), as absolute paths; empty plays the taps. A
    // finished turn without its own file plays soundFile at half volume.
    [[nodiscard]] const QString& alertSoundFile() const { return alert_sound_file_; }
    [[nodiscard]] const QString& finishSoundFile() const { return finish_sound_file_; }
    [[nodiscard]] bool keepAwake() const { return keep_awake_; }
    [[nodiscard]] QVariantMap visual() const;
    // Sets one visual-view key (theme, font, codeFont, size or all) and
    // saves; an invalid value is refused and nothing changes.
    Q_INVOKABLE bool setVisual(const QString& key, const QVariant& value);
    // Light or dark reading: "dark", "light" or "system" (follows macOS).
    // It picks the visual view's theme and the window's from the person's
    // last choice of each kind (visual.darkTheme, lightTheme, darkWindowTheme,
    // lightWindowTheme); terminal colors never change. Choosing a theme of
    // the other kind switches to that kind. Saved; an unknown mode is refused.
    Q_INVOKABLE bool setAppearance(const QString& mode);
    [[nodiscard]] const QString& appearance() const { return appearance_; }
    // Whether light mode shows now (for "system", whether macOS is light).
    [[nodiscard]] bool lightShown() const;
    // macOS's own appearance, which "system" follows. The window feeds it
    // from the platform's color scheme; tests set it directly. Not saved.
    Q_INVOKABLE void setSystemDark(bool dark);
    // A visual-view theme is light when its name ends in "-light".
    [[nodiscard]] static bool visualThemeLight(const QString& name);
    // Sets one setting kept in a section of its own (see `settings`), or
    // harnessUpdates.<cli> (false keeps that CLI's version), and saves. Only
    // the section changed is written; the rest of it (machines, models,
    // providers...) stays as written. An invalid value is refused.
    Q_INVOKABLE bool setSetting(const QString& path, const QVariant& value);
    [[nodiscard]] QVariantMap settings() const;
    // Every setting Settings shows, at its built-in value, by the names
    // visual(), settings() and the properties above use; for Reset.
    Q_INVOKABLE [[nodiscard]] static QVariantMap defaults();
    // visual.keepMB and visual.keepThreads: how much memory and how many
    // conversations the visual view keeps parsed besides the shown one, so
    // switching agents shows their threads at once. Hand-written only.
    [[nodiscard]] int visualKeepMB() const { return visual_keep_mb_; }
    [[nodiscard]] int visualKeepThreads() const { return visual_keep_threads_; }
    [[nodiscard]] bool visualAll() const { return visual_all_; }
    // visual.factory: the window shows the factory view (one orchestrator
    // conversation and the pipeline board) instead of categories.
    [[nodiscard]] bool visualFactory() const { return visual_factory_; }
    [[nodiscard]] const AgentDefaults& agentDefaults() const { return agent_defaults_; }
    // {"nextPrompt": {...}}: when and how lapis predicts the next prompt.
    [[nodiscard]] const NextPromptSettings& nextPrompt() const { return next_prompt_; }
    // {"interactionLog": {...}}: the local log of input in lapis's windows.
    [[nodiscard]] const InteractionLogSettings& interactionLog() const { return interaction_log_; }
    // {"tabAway": {...}}: how Tab ranks the next agent that needs you.
    [[nodiscard]] const TabAwaySettings& tabAway() const { return tab_away_; }
    [[nodiscard]] static int terminalFontSizeMinimum() { return kTerminalFontSizeMinimum; }
    [[nodiscard]] static int terminalFontSizeMaximum() { return kTerminalFontSizeMaximum; }
    [[nodiscard]] static int terminalFontSizeDefault() { return kTerminalFontSizeDefault; }
    // Write the current appearance back to the config file. Returns false and
    // sets diagnostic() when the file cannot be written, leaving the in-memory
    // choice active so the running window still matches what was selected.
    Q_INVOKABLE bool save();

  signals:
    void changed();
    void diagnosticChanged();

  private:
    void apply_defaults();
    void load_terminal_font(const QJsonValue& value);
    void load_harness_arguments(const QJsonValue& value);
    void load_harness_updates(const QJsonValue& value);
    void load_alerts(const QJsonObject& root);
    void load_visual(const QJsonObject& root);
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
    QString chime_diagnostic_;
    CardDensity density_{CardDensity::Comfortable};
    QString theme_{QStringLiteral("lapis")};
    QString terminal_font_family_;
    int terminal_font_size_{kTerminalFontSizeDefault};
    QHash<QString, QStringList> harness_arguments_;
    QSet<QString> harness_updates_off_;
    bool loaded_{};
    bool file_backed_{true};
    bool sidebar_visible_{true};
    bool previews_visible_{false};
    bool finish_sound_{true};
    QString alert_sound_file_;
    QString finish_sound_file_;
    bool keep_awake_{true};
    bool scroll_to_bottom_on_typing_{true};
    QString visual_theme_{QStringLiteral("terminal")};
    QString visual_font_{QStringLiteral("system")};
    QString visual_code_font_{QStringLiteral("system")};
    int visual_size_{};
    bool visual_all_{};
    bool visual_factory_{};
    int visual_keep_mb_{1024};
    int visual_keep_threads_{32};
    // What Command-click on a file path in the visual view opens it with:
    // vscode, cursor, zed, textedit, system, or auto (the first installed).
    QString visual_editor_{QStringLiteral("auto")};
    // Light mode: the mode and the last theme of each kind, for the visual
    // view and the window. visual_theme_ and theme_ hold what shows now.
    QString appearance_{QStringLiteral("dark")};
    bool appearance_chosen_{true};
    QString visual_dark_theme_{QStringLiteral("terminal")};
    QString visual_light_theme_{QStringLiteral("claude-light")};
    QString window_dark_theme_{QStringLiteral("lapis")};
    QString window_light_theme_{QStringLiteral("daylight")};
    bool system_dark_{true};
    // Shows the remembered themes of the kind the mode now asks for.
    void apply_appearance();
    // Remembers a chosen theme as its kind's, switching the mode to that kind
    // when the other kind shows.
    void choose_kind(bool light);
    void follow_system_appearance();
    // The sections setSetting edits, as last read, and the ones it changed
    // since the last save. Pending edits are key-granular so a save preserves
    // concurrent changes to other keys in the same section.
    QJsonObject sections_;
    QSet<QString> touched_;
    QHash<QString, QSet<QString>> touched_keys_;
    void parse_sections();
    AgentDefaults agent_defaults_;
    NextPromptSettings next_prompt_;
    InteractionLogSettings interaction_log_;
    TabAwaySettings tab_away_;
    QFileSystemWatcher watcher_;
    QTimer settle_;
    QByteArray known_contents_;
};
} // namespace lapis::desktop
#endif // LAPIS_DESKTOP_CONFIG_HPP
