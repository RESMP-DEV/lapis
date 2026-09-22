#ifndef LAPIS_DESKTOP_UI_PREVIEW_HPP
#define LAPIS_DESKTOP_UI_PREVIEW_HPP

#include "keymap.hpp"
#include "workspace.hpp"
#include "workspace_supervisor.hpp"
#include <QKeyEvent>
#include <QKeySequence>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <functional>
#include <memory>

class QQmlApplicationEngine;
class QQmlError;
class QQuickWindow;

namespace lapis::desktop {
struct UiPreviewOptions {
    QUrl source;
    bool compact{};
    // Case-insensitive substring of the target QScreen name, for example
    // "built-in" for the MacBook panel and "ultrawide" for an external one.
    // Empty keeps the platform's default placement.
    QString screen;
    // User keybindings and layout, exposed to QML as `keymap`. Optional; a
    // null value uses the shared C++ settings defaults and QML navigation defaults.
    KeyMap* keymap{};
    // Deterministic GUI qualification; production uses the steady clock.
    std::function<qint64()> supervisor_clock{};
};

// View host shared by normal launch and the isolated development fixture.
// Workspace outlives this host. QML types are registered by main before load().
class UiPreview final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool active READ active CONSTANT)
    Q_PROPERTY(
        bool reducedMotion READ reducedMotion WRITE setReducedMotion NOTIFY reducedMotionChanged)
    Q_PROPERTY(bool systemReducedMotion READ systemReducedMotion NOTIFY reducedMotionChanged)
    Q_PROPERTY(QString diagnostics READ diagnostics NOTIFY diagnosticsChanged)
    Q_PROPERTY(QStringList settingsShortcuts READ settingsShortcuts NOTIFY settingsShortcutsChanged)
    Q_PROPERTY(bool holdingKeys READ holdingKeys NOTIFY heldKeysChanged)
    Q_PROPERTY(lapis::desktop::WorkspaceSupervisor* supervisor READ supervisor CONSTANT)
  public:
    UiPreview(Workspace& workspace, UiPreviewOptions options, QObject* parent = nullptr);
    ~UiPreview() override;
    [[nodiscard]] bool active() const { return workspace_.previewMode(); }
    [[nodiscard]] bool reducedMotion() const { return system_reduced_motion_ || reduced_motion_; }
    [[nodiscard]] bool systemReducedMotion() const { return system_reduced_motion_; }
    void setReducedMotion(bool enabled);
    void setSystemReducedMotion(bool enabled);
    [[nodiscard]] const QString& diagnostics() const { return diagnostics_; }
    [[nodiscard]] const QStringList& settingsShortcuts() const { return settings_shortcuts_; }
    [[nodiscard]] bool holdingKeys() const { return !held_keys_.isEmpty(); }
    [[nodiscard]] WorkspaceSupervisor* supervisor() const { return supervisor_.get(); }
    [[nodiscard]] QQuickWindow* window() const;
    bool load();
    Q_INVOKABLE bool reload();
    // Give keyboard ownership to the live session's terminal surface. The
    // surface differs by layout: the single pane in focus mode, the focused
    // tile in blocks mode. Returns true when a terminal took focus.
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
    void heldKeysChanged();
    void windowChanged(QQuickWindow* window);

  private:
    bool eventFilter(QObject* watched, QEvent* event) override;
    bool loadCandidate();
    void refreshSettingsShortcuts();
    void updateWindowActivity(QQuickWindow* window);
    void recordRuntimeWarnings(const QList<QQmlError>& warnings);
    void clearHeldKeys();
    void updateHeldKey(const QKeyEvent& event, bool pressed);
    Workspace& workspace_;
    UiPreviewOptions options_;
    std::unique_ptr<WorkspaceSupervisor> supervisor_;
    std::unique_ptr<QQmlApplicationEngine> engine_;
    QPointer<QQuickWindow> window_;
    QString diagnostics_;
    QStringList settings_shortcuts_;
    QList<QKeySequence> parsed_settings_shortcuts_;
    QSet<int> held_keys_;
    bool publishing_diagnostics_{};
    bool reduced_motion_{};
    bool system_reduced_motion_{};
};
} // namespace lapis::desktop
#endif
