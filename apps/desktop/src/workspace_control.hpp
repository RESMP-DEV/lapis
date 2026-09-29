#ifndef LAPIS_DESKTOP_WORKSPACE_CONTROL_HPP
#define LAPIS_DESKTOP_WORKSPACE_CONTROL_HPP
#include <QByteArray>
#include <QJsonObject>
#include <QLocalServer>
#include <QObject>
#include <QString>

namespace lapis::desktop {
class KeyMap;
class Terminals;
class Workspace;

// Requests to the lapis process that owns the workspace (a window, or the
// windowless host) from other lapis processes on this Mac, such as the phone
// gateway: one JSON line per connection on <registry folder>/
// workspace-control.sock, answered with one JSON line. Version 1 requests:
// "harnesses", "createAgent" (category, harness, directory, optional title and
// resume), "createCategory" (name), "renameCategory" (id, name),
// "removeCategory" (id), "placeCategory" (id, index), "closeAgent" (id),
// "renameAgent" (id, title), "placeAgent" (id, category, index),
// "restartAgent" (id), "openTerminal" (machine, "" for this Mac),
// "closeTerminal" (id), "settings", "changeSettings" (settings: the names
// and values to change) and "handover", which only the windowless host
// honours.
class WorkspaceControl final : public QObject {
    Q_OBJECT
  public:
    WorkspaceControl(Workspace& workspace, bool host, QObject* parent = nullptr);
    ~WorkspaceControl() override;
    WorkspaceControl(const WorkspaceControl&) = delete;
    WorkspaceControl& operator=(const WorkspaceControl&) = delete;
    WorkspaceControl(WorkspaceControl&&) = delete;
    WorkspaceControl& operator=(WorkspaceControl&&) = delete;

    [[nodiscard]] bool listening() const { return server_.isListening(); }
    // Quick-command terminals, for the phone; without them those requests fail.
    void setTerminals(Terminals* terminals) { terminals_ = terminals; }
    // lapis.json, for the settings the phone can change; without it those
    // requests fail.
    void setKeyMap(KeyMap* keymap) { keymap_ = keymap; }
    [[nodiscard]] static QString path(const QString& registry);
    // Asks the windowless host holding this registry to let a window have it.
    static bool requestHandover(const QString& registry);

  signals:
    void handoverRequested();

  private:
    void accept();
    QByteArray answer(const QByteArray& line);
    // Each takes the request's kind and the request.
    QByteArray harnesses(const QString& kind, const QJsonObject& request);
    QByteArray create(const QString& kind, const QJsonObject& request);
    QByteArray category(const QString& kind, const QJsonObject& request);
    QByteArray agent(const QString& kind, const QJsonObject& request);
    QByteArray terminal(const QString& kind, const QJsonObject& request);
    QByteArray settings(const QString& kind, const QJsonObject& request);
    QByteArray handover(const QString& kind, const QJsonObject& request);
    // Refuses with the workspace's reason, which then leaves the window: the
    // phone asked, so the phone shows it.
    QByteArray refused();
    Workspace& workspace_;
    Terminals* terminals_{};
    KeyMap* keymap_{};
    bool host_;
    QLocalServer server_;
};
} // namespace lapis::desktop
#endif
