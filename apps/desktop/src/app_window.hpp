#ifndef LAPIS_DESKTOP_APP_WINDOW_HPP
#define LAPIS_DESKTOP_APP_WINDOW_HPP

#include "config.hpp"
#include "workspace.hpp"
#include <QKeySequence>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QRect>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <memory>

class QQmlApplicationEngine;
class QQmlContext;
class QQmlError;
class QQuickWindow;

namespace lapis::desktop {
struct AppWindowOptions {
    QUrl source;
    bool compact{};
    // Case-insensitive substring of the target QScreen name, for example
    // "built-in" for the MacBook panel and "ultrawide" for an external one.
    // Empty keeps the platform's default placement.
    QString screen;
    // User keybindings and layout, exposed to QML as `config`. Optional; when
    // null, the window owns a defaults-only Config that reads and writes no file.
    Config* config{};
    // The rest are optional services. Each is exposed under its name either
    // way: QML sees the object or null, never undefined.
    QObject* alerts{};        // exposed to QML as `alerts`
    QObject* tools{};         // exposed to QML as `tools`
    QObject* desktop{};       // exposed to QML as `desktop`
    QObject* conversations{}; // exposed to QML as `conversations`
    QObject* terminals{};     // exposed to QML as `terminals`
    QObject* nextPrompt{};    // exposed to QML as `nextPrompt`
    QObject* guiState{};      // exposed to QML as `guiState` (a restart keeps it)
    // Only the normal workspace restores user geometry; tests opt in with an isolated path.
    bool persistGeometry{};
    QString geometryPath{};
    // Closing the window hides it and the app keeps running (the Mac's
    // convention); quitting still closes it.
    bool hideOnClose{};
};

// View host shared by normal launch and the isolated development fixture.
// Workspace outlives this host. QML types are registered by main before load().
class AppWindow final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool active READ active CONSTANT)
    Q_PROPERTY(
        bool reducedMotion READ reducedMotion WRITE setReducedMotion NOTIFY reducedMotionChanged)
    Q_PROPERTY(bool systemReducedMotion READ systemReducedMotion NOTIFY reducedMotionChanged)
    Q_PROPERTY(QString diagnostics READ diagnostics NOTIFY diagnosticsChanged)
    Q_PROPERTY(QStringList settingsShortcuts READ settingsShortcuts NOTIFY settingsShortcutsChanged)
    // Installed fixed-pitch families offered by Appearance, sorted by name.
    Q_PROPERTY(QStringList monospaceFamilies READ monospaceFamilies CONSTANT)
  public:
    AppWindow(Workspace& workspace, AppWindowOptions options, QObject* parent = nullptr);
    ~AppWindow() override;
    [[nodiscard]] bool active() const { return workspace_.fixtureMode(); }
    [[nodiscard]] bool reducedMotion() const { return system_reduced_motion_ || reduced_motion_; }
    [[nodiscard]] bool systemReducedMotion() const { return system_reduced_motion_; }
    void setReducedMotion(bool enabled);
    void setSystemReducedMotion(bool enabled);
    [[nodiscard]] const QString& diagnostics() const { return diagnostics_; }
    [[nodiscard]] const QStringList& settingsShortcuts() const { return settings_shortcuts_; }
    [[nodiscard]] QStringList monospaceFamilies() const;
    [[nodiscard]] QQuickWindow* window() const;
    bool load();
    Q_INVOKABLE bool reload();
    // Give keyboard ownership to the live session's terminal surface. The
    // single stage owns input in every appearance. Returns true when a terminal took focus.
    Q_INVOKABLE bool assignTerminalFocus();
    Q_INVOKABLE void deferTerminalFocus();
    // Open the appearance dialog. Terminal surfaces consume key events before
    // QML Shortcut sees them, so the app-level shortcut is handled here, where
    // it can intercept ahead of any focused item.
    Q_INVOKABLE bool openSettings();
  signals:
    void reducedMotionChanged();
    void diagnosticsChanged();
    void settingsShortcutsChanged();
    void windowChanged(QQuickWindow* window);

  private:
    bool eventFilter(QObject* watched, QEvent* event) override;
    bool loadCandidate();
    void exposeObjects(QQmlContext& context);
    void publishWarnings(const QList<QQmlError>& warnings);
    void configureGeometry(QQuickWindow& target, bool reloading);
    void rememberGeometry();
    void refreshSettingsShortcuts();
    [[nodiscard]] bool geometryPersistenceEnabled() const;
    void restoreGeometry(QQuickWindow& window);
    void saveGeometry();
    QRect normal_geometry_;
    Workspace& workspace_;
    AppWindowOptions options_;
    // Backs options_.config when the caller passed none; outlives engine_.
    std::unique_ptr<Config> defaults_config_;
    std::unique_ptr<QQmlApplicationEngine> engine_;
    QPointer<QQuickWindow> window_;
    QString diagnostics_;
    QStringList settings_shortcuts_;
    QList<QKeySequence> parsed_settings_shortcuts_;
    bool publishing_diagnostics_{};
    bool shutting_down_{};
    bool quitting_{};
    bool reduced_motion_{};
    bool system_reduced_motion_{};
};
} // namespace lapis::desktop
#endif
