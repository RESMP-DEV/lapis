#ifndef LAPIS_DESKTOP_USAGE_HPP
#define LAPIS_DESKTOP_USAGE_HPP
#include <QByteArray>
#include <QDate>
#include <QDateTime>
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <QThreadPool>
#include <QTimer>
#include <QVariantList>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <vector>

namespace lapis::desktop {

// Tokens the way both CLIs report them. Input counts only what did not come
// from the prompt cache; output includes reasoning.
struct TokenCount {
    qint64 input{};
    qint64 cacheRead{};
    qint64 cacheWrite{};
    qint64 output{};
    [[nodiscard]] qint64 total() const { return input + cacheRead + cacheWrite + output; }
    TokenCount& operator+=(const TokenCount& other);
    TokenCount& operator-=(const TokenCount& other);
    bool operator==(const TokenCount&) const = default;
};

// Counts tokens from the Codex and Claude transcripts on this machine. Each
// file is read once and afterwards only what was appended, so a refresh costs
// the new lines. Codex writes a running total per session; Claude writes a
// message's usage on each of its lines, the last with the final output count,
// so each message counts once at its largest.
class TokenLedger {
  public:
    struct Roots {
        QString codex;  // ~/.codex/sessions
        QString claude; // ~/.claude/projects
    };
    // Per provider ("codex", "claude"), per local day, per model.
    using Days = std::map<QDate, QHash<QString, TokenCount>>;

    explicit TokenLedger(Roots roots) : roots_(std::move(roots)) {}
    // Reads what is new in files touched since `since` (a local day).
    void scan(QDate since);
    [[nodiscard]] QHash<QString, Days> days() const;
    [[nodiscard]] qint64 bytesRead() const { return bytes_read_; }

  private:
    struct File {
        qint64 offset{};
        qint64 size{};
        QDateTime modified;
        TokenCount total; // Codex: the session's running total so far
        bool started{};   // Codex: a total has been read
        QString model;    // Codex: the model of the current turn
        QHash<QDate, QHash<QString, TokenCount>> days;
    };
    enum class Source : std::uint8_t { codex, claude };
    void walk(Source source, const QString& root, QHash<QString, File>& files);
    void read(Source source, const QString& path, File& file);
    void codexLine(QByteArrayView line, File& file);
    void claudeLine(QByteArrayView line, File& file);
    [[nodiscard]] QDate localDay(QByteArrayView timestamp);

    Roots roots_;
    QDate since_;
    QHash<QString, File> codex_;
    QHash<QString, File> claude_;
    QHash<quint64, TokenCount> seen_; // counted so far, by Claude message and request
    QHash<qint64, QDate> hours_;      // UTC hour -> local day
    qint64 bytes_read_{};
};

// One plan window, as the CLI reported it.
struct UsageWindow {
    QString label; // "5 hours", "Week", "Week, Fable"
    double percent{};
    QDateTime resets; // invalid when the CLI did not say
    int minutes{};    // the window's length, when known
};

// What each CLI said about its plan: its windows, or why there are none.
struct PlanLimits {
    std::vector<UsageWindow> windows;
    QString plan;       // "pro", "max"
    QString note;       // e.g. "Limit reached", or why nothing is known
    int resetCredits{}; // Codex: free resets on the account
};
[[nodiscard]] PlanLimits codex_limits(const QJsonObject& result);
[[nodiscard]] PlanLimits claude_limits(const QJsonObject& response);

// Plan limits asked of each CLI through its own protocol (Codex app-server's
// account/rateLimits/read, Claude's get_usage control request, which sends
// no prompt and spends nothing), and token totals from the transcripts. Both
// refresh on a timer while active and on request.
class Usage final : public QObject {
    Q_OBJECT
    // [{id, name, plan, note, checked, resetCredits,
    //   windows: [{label, percent, resets, minutes, pace}],
    //   today, month: {total, input, cacheRead, cacheWrite, output},
    //   days: [30 day totals, oldest first], models: [{name, total}] (this month)}]
    Q_PROPERTY(QVariantList providers READ providers NOTIFY changed)
    Q_PROPERTY(bool counting READ counting NOTIFY changed)
  public:
    // The CLI program for a provider id, or empty when it is not installed.
    using Programs = std::function<QString(const QString&)>;
    static constexpr int kLimitsMs = 5 * 60 * 1000;
    static constexpr int kTokensMs = 2 * 60 * 1000;
    static constexpr int kFirstLimitsMs = 2 * 1000;
    static constexpr int kFirstCountMs = 15 * 1000;
    static constexpr int kAnswerMs = 20 * 1000;

    Usage(Programs programs, TokenLedger::Roots roots, QObject* parent = nullptr);
    ~Usage() override;
    Usage(const Usage&) = delete;
    Usage& operator=(const Usage&) = delete;

    // Polls while active; stops, and kills any query, when not.
    void setActive(bool active);
    Q_INVOKABLE void refresh();
    [[nodiscard]] QVariantList providers() const;
    [[nodiscard]] bool counting() const { return counting_; }
    // Tests: the clock that decides today and this month.
    void setTodayForTesting(QDate today) { today_ = today; }

  signals:
    void changed();

  private:
    struct Query {
        QString provider;
        QPointer<QProcess> process; // deleted later, never inside its own signal
        QByteArray pending;
    };
    void askLimits();
    void ask(const QString& provider);
    void answer(const QString& provider);
    void finish(const QString& provider, std::optional<PlanLimits> limits, const QString& failure);
    void count();
    [[nodiscard]] QDate today() const;

    Programs programs_;
    TokenLedger ledger_;
    QThreadPool pool_;
    bool active_{};
    bool counting_{};
    QTimer limits_timer_;
    QTimer tokens_timer_;
    std::vector<Query> queries_;
    QHash<QString, PlanLimits> limits_;
    QHash<QString, QDateTime> checked_;
    QHash<QString, TokenLedger::Days> days_;
    QDate today_;
};
} // namespace lapis::desktop
#endif
