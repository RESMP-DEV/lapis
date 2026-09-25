#ifndef LAPIS_DESKTOP_USAGE_HPP
#define LAPIS_DESKTOP_USAGE_HPP
#include "usage_limits.hpp"
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
#include <QVariantMap>
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

// Tokens counted on another machine by its own python3 (usage/count_tokens.py),
// as {provider: {model: {"YYYY-MM-DDTHH" (UTC): [input, cacheRead,
// cacheWrite, output]}}}, gathered into local days from `since`.
[[nodiscard]] QHash<QString, TokenLedger::Days> remote_days(const QJsonObject& counts, QDate since);

// Usage by machine: this Mac and each ssh host configured. On each machine,
// every CLI lapis knows that is signed in to a plan says how much of it is
// used (see UsageProbe), and on this Mac OMP adds every account its logins
// hold; an account two sources report is shown once. Tokens come from the
// Codex and Claude transcripts on each machine. Everything refreshes on a
// timer while active and on request; a CLI that is not installed or not
// signed in is left out.
class Usage final : public QObject {
    Q_OBJECT
    // This Mac first: [{host, name, note, counting, providers: [{id, name,
    //   counted, accounts: [{label, source, plan, note, resetCredits, checked,
    //   windows: [{label, percent (used), resets, minutes, pace (used by
    //   the reset at this pace), runsOut (when, if before the reset)}]}],
    //   today, month: {total, input, cacheRead, cacheWrite, output},
    //   days: [30 day totals, oldest first], models: [{name, total}]}]}]
    Q_PROPERTY(QVariantList machines READ machines NOTIFY changed)
    // Under the categories: this Mac's plans, each at its tightest window:
    // [{id, name, percent, label}]
    Q_PROPERTY(QVariantList meter READ meter NOTIFY changed)
    Q_PROPERTY(bool counting READ counting NOTIFY changed)
  public:
    // The program for a CLI id, or "ssh", on this Mac; empty when missing.
    using Programs = std::function<QString(const QString&)>;
    static constexpr int kLimitsMs = 5 * 60 * 1000;
    static constexpr int kTokensMs = 2 * 60 * 1000;
    static constexpr int kRemoteTokensMs = 30 * 60 * 1000;
    static constexpr int kFirstLimitsMs = 2 * 1000;
    static constexpr int kFirstCountMs = 15 * 1000;
    static constexpr int kFirstRemoteCountMs = 30 * 1000;
    static constexpr int kRemoteCountMs = 180 * 1000;
    static constexpr int kProbesAtOnce = 3;

    Usage(Programs programs, TokenLedger::Roots roots, QObject* parent = nullptr);
    ~Usage() override;
    Usage(const Usage&) = delete;
    Usage& operator=(const Usage&) = delete;

    // Polls while active; stops, and ends every check, when not.
    void setActive(bool active);
    // The other machines with a dashboard, as ssh names them.
    void setMachines(const QStringList& hosts);
    // The meter's plans in this order; empty shows every signed-in plan.
    void setMeterOrder(const QStringList& providers);
    Q_INVOKABLE void refresh();
    [[nodiscard]] QVariantList machines() const;
    [[nodiscard]] QVariantList meter() const;
    [[nodiscard]] bool counting() const;
    // Tests: the clock that decides today and this month.
    void setTodayForTesting(QDate today) { today_ = today; }

  signals:
    void changed();

  private:
    struct Machine {
        QString host;                      // empty: this Mac
        QHash<QString, PlanLimits> logins; // each CLI's own sign-in, by CLI
        QHash<QString, QDateTime> checked;
        std::vector<PlanLimits> accounts; // OMP's
        QHash<QString, TokenLedger::Days> days;
        QString note; // why it could not be reached
        QPointer<QProcess> counter;
        QDateTime counted;
        QByteArray output;
    };
    struct Check {
        QString host;
        QString provider;
    };
    struct Running {
        Check check;
        QPointer<UsageProbe> probe;
    };
    void askLimits();
    void schedule();
    void probed(const Check& check, const UsageProbe::Result& result);
    void count();
    void countRemote(const QString& host);
    [[nodiscard]] Machine* find(const QString& host);
    [[nodiscard]] QDate today() const;
    [[nodiscard]] QDate since() const;
    [[nodiscard]] QStringList providerOrder(const Machine& machine) const;
    [[nodiscard]] QVariantMap providerEntry(const Machine& machine, const QString& id,
                                            const QDateTime& now) const;

    Programs programs_;
    TokenLedger ledger_;
    QThreadPool pool_;
    bool active_{};
    bool counting_{};
    QTimer limits_timer_;
    QTimer tokens_timer_;
    QTimer remote_timer_;
    std::vector<Machine> machines_; // this Mac first
    QStringList meter_order_;
    std::vector<Check> queue_;
    std::vector<Running> running_;
    QDate today_;
};
} // namespace lapis::desktop
#endif
