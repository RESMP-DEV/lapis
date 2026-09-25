#ifndef LAPIS_DESKTOP_USAGE_LIMITS_HPP
#define LAPIS_DESKTOP_USAGE_LIMITS_HPP
#include <QByteArray>
#include <QDateTime>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <functional>
#include <optional>
#include <vector>

namespace lapis::desktop {

// One plan window, as a CLI reported it.
struct UsageWindow {
    QString label{}; // "5 hours", "Week", "Week, Fable"
    double percent{};
    QDateTime resets{}; // invalid when the CLI did not say
    int minutes{};      // the window's length, when known
};

// One account's plan: its windows, or why there are none.
struct PlanLimits {
    std::vector<UsageWindow> windows{};
    QString provider{};  // codex, claude, grok, kimi, or another plan OMP knows
    QString source{};    // the CLI that answered: codex, claude, grok, kimi or omp
    QString account{};   // who, when the source says (an email)
    QString accountId{}; // for recognizing one account across sources
    QString plan{};      // "pro", "max"
    QString note{};      // e.g. "Limit reached", or why nothing is known
    int resetCredits{};  // free resets on the account
};

// The answers of each CLI's own usage interface.
[[nodiscard]] PlanLimits codex_limits(const QJsonObject& result);
[[nodiscard]] PlanLimits claude_limits(const QJsonObject& response);
[[nodiscard]] PlanLimits grok_limits(const QJsonObject& billing);
[[nodiscard]] PlanLimits kimi_limits(const QJsonObject& data);
[[nodiscard]] std::vector<PlanLimits> omp_limits(const QJsonObject& result);
// Whether two answers describe the same account: the same account ID, or a
// window of the same length ending at the same moment at the same use.
[[nodiscard]] bool same_account(const PlanLimits& a, const PlanLimits& b);
// The words of a POSIX shell command, each quoted.
[[nodiscard]] QString shell_words(const QStringList& words);

// Asks one CLI, on this Mac or over ssh, how much of its plan is used,
// through the CLI's own interface and without a prompt:
//   codex   app-server account/rateLimits/read
//   claude  the get_usage control request, with no settings (so none of the
//           person's hooks run) and no transcript written
//   grok    ACP _x.ai/billing from `grok agent stdio`, not joining a leader
//   kimi    GET /api/v1/oauth/usage from a short-lived `kimi web` on loopback
//   omp     ACP _omp/usage: every account OMP's logins hold, with its session
//           kept in a folder deleted afterwards
// Another machine runs the CLI through `ssh -T -o BatchMode=yes` in an
// interactive login shell, so its PATH matches that machine's terminal.
class UsageProbe final : public QObject {
    Q_OBJECT
  public:
    struct Setup {
        QString provider; // codex, claude, grok, kimi or omp
        QString program;  // the CLI here, or its name there
        QString host;     // empty: this Mac
        QString ssh;      // the ssh program, for a host
    };
    // `limits` on an answer; otherwise why not, and whether the CLI is simply
    // not installed or not signed in there.
    struct Result {
        std::optional<std::vector<PlanLimits>> limits;
        QString failure;
        bool absent{};
    };
    using Done = std::function<void(const Result&)>;
    static constexpr int kAnswerMs = 25 * 1000;

    UsageProbe(Setup setup, Done done, QObject* parent = nullptr);
    ~UsageProbe() override;
    UsageProbe(const UsageProbe&) = delete;
    UsageProbe& operator=(const UsageProbe&) = delete;
    void start();

  private:
    void read();
    void line(const QJsonObject& message);
    void kimiOutput();
    void finish(const Result& result);
    void send(const QJsonObject& message);

    Setup setup_;
    Done done_;
    QTemporaryDir scratch_;
    QPointer<QProcess> process_;
    QNetworkAccessManager network_;
    QByteArray pending_;
    QByteArray text_; // kimi: its startup output, for the port's token
    int port_{};
    bool asked_{};
    bool finished_{};
};
} // namespace lapis::desktop
#endif
