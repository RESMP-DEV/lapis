#ifndef LAPIS_DESKTOP_TERMINALS_HPP
#define LAPIS_DESKTOP_TERMINALS_HPP
#include "launch_spec.hpp"
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <memory>
#include <vector>

namespace lapis::desktop {
class SessionPreview;

// The ssh hosts named in an ssh config (following Include), without patterns.
[[nodiscard]] QStringList ssh_config_hosts(const QString& path);

// Plain shells for a quick command, apart from agents: one per machine (this
// Mac, or an ssh host), never in a category, never an attention source. Each
// runs under its own session service like an agent does, so closing lapis
// leaves it running and the next lapis reattaches it; terminals.json beside
// the workspace records them for that and for the phone's gateway.
class Terminals final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList machines READ machines NOTIFY machinesChanged)
    Q_PROPERTY(lapis::desktop::SessionPreview* current READ current NOTIFY currentChanged)
    Q_PROPERTY(QString currentMachine READ currentMachine NOTIFY currentChanged)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)
  public:
    // `runtime` is the workspace's folder; `ssh_config` lists the machines.
    Terminals(QString runtime, QString ssh_config, QObject* parent = nullptr);
    ~Terminals() override;
    // {id ("" for this Mac), name, open}: this Mac first, then ssh hosts.
    [[nodiscard]] QVariantList machines() const;
    [[nodiscard]] SessionPreview* current() const;
    [[nodiscard]] const QString& currentMachine() const { return current_machine_; }
    [[nodiscard]] const QString& error() const { return error_; }
    // Shows the machine's terminal, starting a shell there when it has none
    // or its shell ended. False with `error` when it cannot start.
    Q_INVOKABLE bool show(const QString& machine);
    // The machine's terminal id, starting one when needed (for the phone).
    QString open(const QString& machine);
    // Ends a terminal's shell; its entry goes when the shell has exited.
    Q_INVOKABLE bool close(const QString& id);
    // Reattaches the terminals whose services still run.
    void restore();
    [[nodiscard]] SessionPreview* terminal(const QString& id) const;
    [[nodiscard]] QString registryPath() const;
    // Tests: the shell a local terminal runs.
    void setShellForTesting(const QString& program) { shell_ = program; }
  signals:
    void machinesChanged();
    void currentChanged();
    void errorChanged();

  private:
    struct Entry {
        QString id;
        QString machine;
        QString endpoint;
        session::LaunchSpec launch;
        std::unique_ptr<SessionPreview> session;
    };
    [[nodiscard]] session::LaunchSpec launchFor(const QString& machine) const;
    Entry* find(const QString& machine);
    Entry* start(const QString& machine);
    void attach(Entry& entry, bool create);
    void watch(Entry& entry);
    void discard(const QString& id);
    void save() const;
    bool fail(const QString& message);
    QString runtime_;
    QString ssh_config_;
    QString shell_;
    std::vector<Entry> entries_;
    QString current_machine_;
    QString error_;
};
} // namespace lapis::desktop
#endif
