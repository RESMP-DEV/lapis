#ifndef LAPIS_DESKTOP_NEXT_PROMPT_HPP
#define LAPIS_DESKTOP_NEXT_PROMPT_HPP

#include <QElapsedTimer>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QString>
#include <deque>
#include <functional>
#include <optional>

namespace lapis::session {
struct TerminalSnapshot;
}

namespace lapis::desktop {

// {"nextPrompt": {"auto": true, "model": "claude-opus-5-5", "effort": "",
// "minConfidence": 0.5, "maxPerHour": 60}}. Off unless asked for: each
// prediction is a model call on the Claude Code plan this Mac is signed in to.
struct NextPromptSettings {
    bool automatic{false};
    QString model{QStringLiteral("claude-opus-5-5")};
    QString effort; // empty: the CLI's own
    double minConfidence{0.5};
    int maxPerHour{60};
    bool operator==(const NextPromptSettings&) const = default;
};
[[nodiscard]] NextPromptSettings parse_next_prompt(const QJsonValue& value);

// A screen's text, a row a line, without trailing blanks or blank last rows.
[[nodiscard]] QString terminal_screen_text(const session::TerminalSnapshot& snapshot);

// What the person will likely type next to a Claude Code or Codex agent that
// just finished a turn, as Cursor's Tab predicts an edit. The helper
// (next_prompt.py) reads the conversation where the agent runs, then a model
// on this Mac sees it with the person's standing instructions, the agent's
// screen and every other agent's state. A guess at least `minConfidence`
// likely is offered, dim at the agent's cursor; Tab types it and
// Command-Return sends it. Nothing is ever sent without that key.
//
// Every prediction and what became of it goes to a private log (JSON lines,
// owner-only), which scripts/next_prompt_eval.py joins with what the person
// actually typed: the measure of this, and the data for a model of one's own.
class NextPrompt final : public QObject {
    Q_OBJECT
    // Changes with every suggestion offered or withdrawn, for QML bindings.
    Q_PROPERTY(int revision READ revision NOTIFY changed)
  public:
    struct Agent {
        QString machine;      // an ssh host; empty for this Mac
        QString folder;       // its folder there
        QString cli;          // "claude" or "codex"
        QString conversation; // the CLI's id for it, when lapis knows it
        QString title;
        QString category;
        QString screen; // what its terminal shows now
    };
    using Lookup = std::function<std::optional<Agent>(const QString& id)>;
    // One line of state for every agent: {title, category, status, waiting}.
    using Agents = std::function<QJsonArray()>;
    // The path of "python3", "ssh" or "claude" on this Mac.
    using Program = std::function<QString(const QString&)>;
    // Where the helper is written (a private runtime folder) and the log.
    struct Files {
        QString folder;
        QString log;
    };
    NextPrompt(Lookup lookup, Agents agents, Program program, const Files& files,
               QObject* parent = nullptr);
    ~NextPrompt() override;
    NextPrompt(const NextPrompt&) = delete;
    NextPrompt& operator=(const NextPrompt&) = delete;

    void setSettings(NextPromptSettings settings);
    [[nodiscard]] const NextPromptSettings& settings() const { return settings_; }
    // The agent finished a turn: withdraw its suggestion and predict anew.
    void turnFinished(const QString& id);
    // The suggestion offered for the agent, or empty.
    Q_INVOKABLE [[nodiscard]] QString suggestion(const QString& id) const;
    // The person took it: typed into the agent, and `sent` when submitted.
    Q_INVOKABLE void used(const QString& id, bool sent);
    // The person typed something else.
    Q_INVOKABLE void dismiss(const QString& id);
    [[nodiscard]] int revision() const { return revision_; }

  signals:
    void changed();

  private:
    struct Run {
        quint64 generation{};
        Agent agent;
        QPointer<QProcess> process;
    };
    struct Offer {
        QString text;
        QString conversation;
        int turn{};
    };
    void start(const QString& id, quint64 generation, const QString& program,
               const QStringList& arguments, const QByteArray& input, int timeout_ms,
               const std::function<void(const QJsonObject&)>& done);
    void predict(const QString& id, quint64 generation, const QJsonObject& context);
    void offer(const QString& id, const Agent& agent, const QJsonObject& context,
               const QJsonObject& answer);
    void withdraw(const QString& id);
    void record(QJsonObject event) const;
    [[nodiscard]] bool current(const QString& id, quint64 generation) const;
    Lookup lookup_;
    Agents agents_;
    Program program_;
    QString script_path_;
    QString log_path_;
    NextPromptSettings settings_;
    QHash<QString, Run> running_; // by agent id
    QHash<QString, Offer> offers_;
    std::deque<qint64> started_; // for maxPerHour
    QElapsedTimer clock_;
    quint64 generation_{};
    int revision_{};
};

} // namespace lapis::desktop
#endif
