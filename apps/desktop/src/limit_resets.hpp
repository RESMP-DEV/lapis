#ifndef LAPIS_DESKTOP_LIMIT_RESETS_HPP
#define LAPIS_DESKTOP_LIMIT_RESETS_HPP

#include <QHash>
#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <functional>
#include <utility>

namespace lapis::desktop {

// {"limitResets": {"auto": true, "minBlockedMinutes": 60, "keepCredits": 0,
// "salvageHours": 12}}: OMP's defaults.
struct LimitResetSettings {
    bool automatic{true};
    double minBlockedMinutes{60};
    int keepCredits{0};
    double salvageHours{12};
    bool operator==(const LimitResetSettings&) const = default;
};
[[nodiscard]] LimitResetSettings parse_limit_resets(const QJsonValue& value);

// Spends saved Claude Code and Codex limit resets as OMP does, on each machine
// where agents of those CLIs run, with the sign-in each CLI keeps there: every
// five minutes, a reset that restores a long block or would expire unspent
// (limit_resets.py holds the rules), and at once for an agent when
// asked, as claude.ai's "Reset for free" does, with that agent's plan and no
// other CLI's. A key the helper reports as settled is not tried again for eight
// days (the longest window); one the account had nothing to reset for, a
// throttle, a server error or a lost answer is retried after an hour.
class LimitResets final : public QObject {
    Q_OBJECT
  public:
    // The machines ("" for this Mac) running Claude Code or Codex agents.
    using Machines = std::function<QStringList()>;
    // An agent's machine, its CLI ("claude" or "codex"; empty otherwise) and,
    // when it runs on a plan lapis keeps a credential for, where that is on
    // its machine: a Claude Code token file or a Codex home. Empty means the
    // machine's own sign-in.
    struct AgentPlan {
        QString machine;
        QString cli;
        QString credential;
    };
    using Agent = std::function<AgentPlan(const QString& id)>;
    // The path of "python3" or "ssh" on this Mac.
    using Program = std::function<QString(const QString&)>;
    // Claude Code's stored credentials on this Mac (its keychain item); may
    // block on the system's permission prompt, so it runs off the main thread.
    using Credentials = std::function<QByteArray()>;
    LimitResets(Machines machines, Agent agent, Program program, Credentials credentials,
                const QString& folder, QObject* parent = nullptr);
    ~LimitResets() override;
    LimitResets(const LimitResets&) = delete;
    LimitResets& operator=(const LimitResets&) = delete;

    void setSettings(const LimitResetSettings& settings);
    void setInterval(int ms) { timer_.setInterval(ms); }
    // Runs a sweep now, on every machine with Claude Code or Codex agents.
    void sweep();
    Q_INVOKABLE [[nodiscard]] bool canUseNow(const QString& id) const;
    // Spends the saved reset of the agent's plan, on its machine.
    Q_INVOKABLE void useNow(const QString& id);

  signals:
    // A reset was spent; `why` is "blocked", "expiring" or "asked".
    void spent(const QString& machine, const QString& cli, const QString& email,
               const QString& title, const QString& why);
    // A reset asked for could not be spent.
    void declined(const QString& machine, const QString& cli, const QString& reason);

  private:
    // One run of the helper: on a machine ("" for this Mac), and for a CLI
    // whose reset was asked for at once with its plan's credential (both empty
    // in a sweep).
    struct Check {
        QString machine;
        QString asked;
        QString credential;
    };
    void run(const Check& check);
    void start(const Check& check, const QByteArray& credentials);
    void finish(const Check& check, QProcess* process);
    void note(const Check& check, const QJsonObject& account, qint64 nowMs);
    [[nodiscard]] QStringList arguments(const Check& check, bool withCredentials) const;
    Machines machines_;
    Agent agent_;
    Program program_;
    Credentials credentials_;
    QString script_path_;
    LimitResetSettings settings_;
    QTimer timer_;
    QHash<QString, QPointer<QProcess>> running_;       // by machine
    QHash<QString, QHash<QString, qint64>> attempted_; // machine -> key -> until (ms)
};

} // namespace lapis::desktop
#endif
