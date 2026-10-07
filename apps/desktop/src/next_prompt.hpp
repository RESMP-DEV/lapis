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
    // The agent finished a turn: withdraw its suggestion and predict anew.
    void turnFinished(const QString& id);
    // The suggestion offered for the agent, or empty.
    Q_INVOKABLE [[nodiscard]] QString suggestion(const QString& id) const;
    // The agents with a suggestion offered, each true once it was seen.
    Q_INVOKABLE [[nodiscard]] QVariantMap readyAgents() const;
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
    // What a restarted window needs to carry on (see GuiState): the offers
    // shown, each with its key, conversation, turn and whether it was seen;
    // the guesses whose outcome is still to be read from the conversation;
    // and the agents a guess was still being made for.
    [[nodiscard]] QJsonObject saveState() const;
    // Restores that state for agents that still exist, in the same
    // conversation. An offer shows again only once its conversation, read
    // where the agent runs, is still at the offer's turn; otherwise it is
    // recorded as withdrawn. A guess still owed is made again once the agent's
    // screen is back. Restoring offers nothing to send and pings nothing.
    void restoreState(const QJsonObject& state);

  signals:
    void changed();
    // Anything saveState() returns changed, including what QML never shows.
    void stateChanged();

  private:
    struct Run {
        quint64 generation{};
        Agent agent;
        QPointer<UpdaterProcess> process;
        bool verifying{}; // reading the conversation for a restored offer
    };
    struct Offer {
        QString key; // "<agent>:<launch>.<n>", on each of its log records
        QString text;
        QString conversation;
        int turn{};
        qint64 seen_ms{}; // when first on screen; 0 while unseen
    };
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
    // Reading the conversation where the agent runs, then the model here;
    // or reading it to check a restored offer.
    enum class Stage : std::uint8_t { context, predict, verify };
    void start(const QString& id, quint64 generation, const QString& program,
               const QStringList& arguments, const QByteArray& input, Stage stage,
               const std::function<void(const QJsonObject&)>& done);
    void failed(const QString& id, const Agent& agent, Stage stage, const QString& why);
    void predict(const QString& id, quint64 generation, const QJsonObject& context);
    void offer(const QString& id, const Agent& agent, const QJsonObject& context,
               const QJsonObject& answer);
    // Why an offer went unused: the next turn's guess replaced it, or the
    // setting was turned off.
    enum class Withdrawal : std::uint8_t { next_turn, off };
    void withdraw(const QString& id, Withdrawal why);
    void recordWithdrawn(const Offer& offer, const QString& id, const char* reason) const;
    // The conversation's arguments for the helper's context mode.
    [[nodiscard]] QStringList contextWords(const QString& id, const Agent& agent) const;
    void runContext(const QString& id, quint64 generation, const Agent& agent, Stage stage,
                    const std::function<void(const QJsonObject&)>& done);
    // A restored offer: shown once its conversation proves it current.
    void verify(const QString& id, const Agent& agent, const Offer& offer);
    void confirm(const QString& id, const QJsonObject& context);
    void dropRestored(const QString& id, const char* reason);
    // Parts of restoreState: a saved offer, if well formed; the agent it may
    // be restored for; and each saved map. True when anything came back.
    [[nodiscard]] static std::optional<Offer> savedOffer(const QString& id,
                                                         const QJsonValue& value);
    [[nodiscard]] std::optional<Agent> restorable(const QString& id,
                                                  QStringView conversation) const;
    bool restoreAwaiting(const QJsonObject& awaiting);
    bool restoreOffers(const QJsonObject& offers);
    bool restoreOwed(const QJsonObject& owed);
    // A restored owed guess, made once the agent's screen is back.
    void resumeOwed(const QString& id, int tries);
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
    QHash<QString, Offer> restoring_;   // restored offers being checked
    QHash<QString, QString> owed_;      // restored owed guesses: their conversation
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
