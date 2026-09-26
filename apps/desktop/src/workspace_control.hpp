#ifndef LAPIS_DESKTOP_WORKSPACE_CONTROL_HPP
#define LAPIS_DESKTOP_WORKSPACE_CONTROL_HPP
#include <QByteArray>
#include <QJsonObject>
#include <QLocalServer>
#include <QObject>
#include <QString>

namespace lapis::desktop {
class Terminals;
class Workspace;

// Requests to the lapis process that owns the workspace (a window, or the
// windowless host) from other lapis processes on this Mac, such as the phone
// gateway: one JSON line per connection on <registry folder>/
// workspace-control.sock, answered with one JSON line. Version 1 requests:
// "harnesses", "createAgent" (category, harness, directory, optional title and
// resume), "createCategory", "closeAgent", "openTerminal" (machine, "" for this
// Mac), "closeTerminal" (id) and "handover", which only the windowless host
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
    [[nodiscard]] static QString path(const QString& registry);
    // Asks the windowless host holding this registry to let a window have it.
    static bool requestHandover(const QString& registry);

  signals:
    void handoverRequested();

  private:
    void accept();
    QByteArray answer(const QByteArray& line);
    QByteArray create(const QJsonObject& request);
    QByteArray terminal(const QString& kind, const QJsonObject& request);
    Workspace& workspace_;
    Terminals* terminals_{};
    bool host_;
    QLocalServer server_;
};
} // namespace lapis::desktop
#endif
