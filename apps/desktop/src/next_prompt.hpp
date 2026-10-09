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
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>

namespace lapis::session {
struct TerminalSnapshot;
}

namespace lapis::desktop {

// {"nextPrompt": {"auto": true, "model": "claude-opus-5-5", "effort": "",
// "minConfidence": 0, "maxPerHour": 60}}. Off unless asked for: each
// prediction is a model call on the Claude Code plan this Mac is signed in to.
struct NextPromptSettings {
    bool automatic{false};
    QString model{QStringLiteral("claude-opus-5-5")};
    QString effort;            // empty: the CLI's own
    double minConfidence{0.0}; // every probability-bearing guess is offered
    int maxPerHour{60};
    // Experiments: this share of guesses goes to another arm, picked evenly
    // from `experimentModels` (other Claude models, through the same CLI)
    // and, when `localEndpoint` is set, a local model behind an
    // OpenAI-compatible endpoint. Each guess records its arm, so arms can be
    // compared on what the person then sent.
    double experimentShare{0.2};
    QStringList experimentModels{QStringLiteral("claude-sonnet-5-5")};
    QString localEndpoint; // e.g. http://host:8000/v1; empty: no local arm
    QString localModel;
    bool operator==(const NextPromptSettings&) const = default;
};
[[nodiscard]] NextPromptSettings parse_next_prompt(const QJsonValue& value);

// A screen's text, a row a line, without trailing blanks or blank last rows.
[[nodiscard]] QString terminal_screen_text(const session::TerminalSnapshot& snapshot);

// What the person will likely type next to a Claude Code or Codex agent that
// just finished a turn, as Cursor's Tab predicts an edit. The helper
// (next_prompt.py) reads the conversation where the agent runs, then a model
// on this Mac sees it with the person's standing instructions, the agent's
// screen and every other agent's state. The top guess, when it has a usable
// probability of at least `minConfidence`, is offered, dim at the agent's
// cursor; Tab types it for the person to edit. A second Tab can send that
// unchanged guess (see TerminalSurface).
//
// Every prediction and what became of it (seen, used, replaced, and the
// prompt the person then sent, compared with the guess) goes to a
// private log (JSON lines, owner-only), which scripts/next_prompt_eval.py
// turns into use rates and settled-outcome counts, and joins unused offers
// with what the person actually typed.
class UpdaterProcess;

class NextPrompt final : public QObject {
    Q_OBJECT
    // Changes with every suggestion offered or withdrawn, for QML bindings.
    Q_PROPERTY(int revision READ revision NOTIFY changed)
    Q_PROPERTY(bool enabled READ enabled NOTIFY changed)
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
    // The agent finished a turn: withdraw its suggestion and predict anew,
    // unless it is a repeat: no new prompt from the person since the last
    // guess, and that guess was never seen (an agent waking itself). A repeat
    // is guessed when the person next shows that agent (focused).
    void turnFinished(const QString& id);
    // The person shows this agent: a guess deferred as a repeat runs now.
    void focused(const QString& id);
    // The shown guess's probability (0 to 1), for how boldly it is drawn.
    Q_INVOKABLE [[nodiscard]] double confidence(const QString& id) const;
    // At or above this, the guess is drawn bright: the model's own score
    // separated taken guesses (65%) from the rest (25 to 34%), Oct 3 to 9.
    static constexpr double confident = 0.45;
    // The suggestion offered for the agent, or empty.
    Q_INVOKABLE [[nodiscard]] QString suggestion(const QString& id) const;
    // The agents with a suggestion offered, each true once it was seen.
    Q_INVOKABLE [[nodiscard]] QVariantMap readyAgents() const;
    // The offer shown for the agent, for readers outside this window
    // (AgentStatePublisher): {key, text, seen, said}, where `said` is the
    // agent's last reply the guess answers, at most `said_limit` characters.
    // Empty when nothing is offered.
    static constexpr qsizetype said_limit = 600;
    [[nodiscard]] QJsonObject offerState(const QString& id) const;
    // The id of the offer shown for the agent, or empty.
    Q_INVOKABLE [[nodiscard]] QString offerKey(const QString& id) const;
    // The suggestion is on screen in the active window: an impression.
    Q_INVOKABLE void seen(const QString& id);
    Q_INVOKABLE void seenOffer(const QVariantMap& identity);
    // The person took it: typed into the agent (`sent` false), then perhaps
    // sent by a second Tab (`sent` true, a second call); `typedFirst` keys went
    // to the agent while it was offered.
    // Typing is not a refusal: an offer stays until used or replaced by the
    // next turn's. What they then send is recorded as the offer's outcome.
    Q_INVOKABLE void used(const QString& id, bool sent, int typedFirst,
                          const QString& expectedKey = {});
    [[nodiscard]] int revision() const { return revision_; }
    [[nodiscard]] bool enabled() const { return settings_.automatic; }

  signals:
    void changed();
    // An offer was first seen; readyAgents() changed without a new revision.
    void seenChanged();
    // The guessing model's judgement of whether the agent needs the person
    // after a turn: "needs", "steer" or "fyi"; empty when there is none
    // (no guess was made, or it failed). One per turnFinished.
    void judged(const QString& id, const QString& attention);

  private:
    struct Run {
        quint64 generation{};
        Agent agent;
        QPointer<UpdaterProcess> process;
    };
    struct Offer {
        QString key; // "<agent>:<launch>.<n>", on each of its log records
        QString text;
        QString conversation;
        int turn{};
        qint64 seen_ms{}; // when first on screen; 0 while unseen
        QString said;     // the agent's last reply, clipped to said_limit
        double p{};       // the model's probability for it
    };
    // The last guess made for an agent, to tell a repeat (see turnFinished).
    struct Previous {
        QString conversation;
        int turn{-1};
        bool seen{};
    };
    QHash<QString, Previous> previous_;
    QSet<QString> deferred_;
    void run(const QString& id, bool force);
    // The last offer shown to an agent, until the prompt the person sends
    // after it is read from the conversation.
    struct Awaiting {
        Offer offer;
        bool filled{};   // Tab typed it
        bool tab_sent{}; // and a second Tab sent it
    };
    [[nodiscard]] static QJsonObject about(const Offer& offer, const QString& id);
    // Records what the person sent after the agent's last offer, once the
    // conversation (`context`) holds it.
    void settle(const QString& id, const QJsonObject& context);
    // Reading the conversation where the agent runs, then the model here.
    enum class Stage : std::uint8_t { context, predict };
    void start(const QString& id, quint64 generation, const QString& program,
               const QStringList& arguments, const QByteArray& input, Stage stage,
               const std::function<void(const QJsonObject&)>& done);
    void failed(const QString& id, const Agent& agent, Stage stage, const QString& why);
    void predict(const QString& id, quint64 generation, const QJsonObject& context);
    void offer(const QString& id, const Agent& agent, const QJsonObject& context,
               const QJsonObject& answer, const QString& arm, const QString& model);
    // Why an offer went unused: the next turn's guess replaced it, or the
    // setting was turned off.
    enum class Withdrawal : std::uint8_t { next_turn, off };
    void withdraw(const QString& id, Withdrawal why);
    void record(QJsonObject event) const;
    bool budgetAvailable(const QString& id);
    [[nodiscard]] bool current(const QString& id, quint64 generation) const;
    Lookup lookup_;
    Agents agents_;
    Program program_;
    QString script_path_;
    QString log_path_;
    NextPromptSettings settings_;
    QHash<QString, Run> running_; // by agent id
    QHash<QString, Offer> offers_;
    QHash<QString, Awaiting> awaiting_; // by agent id
    struct Attempt {
        qint64 at;
        quint64 generation;
    };
    std::deque<Attempt> started_; // reserved prediction attempts for maxPerHour
    QElapsedTimer clock_;
    quint64 generation_{};
    QString run_; // this launch, in offer ids
    quint64 offers_made_{};
    int revision_{};
};

} // namespace lapis::desktop
#endif
