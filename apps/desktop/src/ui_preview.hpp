#ifndef LAPIS_DESKTOP_UI_PREVIEW_HPP
#define LAPIS_DESKTOP_UI_PREVIEW_HPP

#include "keymap.hpp"
#include "workspace.hpp"
#include <QObject>
#include <QPointer>
#include <QString>
#include <QUrl>
#include <memory>

class QQmlApplicationEngine;
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
    // null value keeps the built-in shortcuts that Main.qml defines itself.
    KeyMap* keymap{};
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
  public:
    UiPreview(Workspace& workspace, UiPreviewOptions options, QObject* parent = nullptr);
    ~UiPreview() override;
    [[nodiscard]] bool active() const { return workspace_.previewMode(); }
    [[nodiscard]] bool reducedMotion() const { return system_reduced_motion_ || reduced_motion_; }
    [[nodiscard]] bool systemReducedMotion() const { return system_reduced_motion_; }
    void setReducedMotion(bool enabled);
    void setSystemReducedMotion(bool enabled);
    [[nodiscard]] const QString& diagnostics() const { return diagnostics_; }
    [[nodiscard]] QQuickWindow* window() const;
    bool load();
    Q_INVOKABLE bool reload();
    // Give keyboard ownership to the live session's terminal surface. The
    // surface differs by layout: the single pane in focus mode, the focused
    // tile in blocks mode. Returns true when a terminal took focus.
    Q_INVOKABLE bool assignTerminalFocus();
  signals:
    void reducedMotionChanged();
    void diagnosticsChanged();
    void windowChanged(QQuickWindow* window);

  private:
    bool loadCandidate();
    Workspace& workspace_;
    UiPreviewOptions options_;
    std::unique_ptr<QQmlApplicationEngine> engine_;
    QPointer<QQuickWindow> window_;
    QString diagnostics_;
    bool reduced_motion_{};
    bool system_reduced_motion_{};
};
} // namespace lapis::desktop
#endif
