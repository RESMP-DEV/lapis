#ifndef LAPIS_DESKTOP_LIMIT_RESETS_HPP
#define LAPIS_DESKTOP_LIMIT_RESETS_HPP

#include <QHash>
#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVector>
#include <cstdint>
#include <functional>
#include <memory>

namespace lapis::desktop {
class UpdaterProcess;

struct LimitResetSettings {
    bool automatic{true};
    double minBlockedMinutes{60};
    int keepCredits{0};
    double salvageHours{12};
    bool operator==(const LimitResetSettings&) const = default;
};
[[nodiscard]] LimitResetSettings parse_limit_resets(const QJsonValue& value);

// The main machine owns durable reset admission. Prepare is read-only; a
// selected credit and operation are persisted before consume. An interrupted
// consume is reconciled without automatically repeating the provider request.
class LimitResets final : public QObject {
    Q_OBJECT
  public:
    struct AgentTarget {
        QString machine;
        QString cli;
        QString account;
        QString home;
        bool hasHome{};
        QString email;
        QString refusal;
        QString credential{};
    };
    using Agents = std::function<QVector<AgentTarget>()>;
    using Agent = std::function<AgentTarget(const QString& id)>;
    using Program = std::function<QString(const QString&)>;
    using Credentials = std::function<QByteArray()>;
    LimitResets(Agents agents, Agent agent, Program program, Credentials credentials,
                const QString& folder, QObject* parent = nullptr);
    ~LimitResets() override;
    LimitResets(const LimitResets&) = delete;
    LimitResets& operator=(const LimitResets&) = delete;

    void setSettings(const LimitResetSettings& settings);
    void setInterval(int ms) { timer_.setInterval(ms); }
    void sweep();
    Q_INVOKABLE [[nodiscard]] bool canUseNow(const QString& id) const;
    Q_INVOKABLE void useNow(const QString& id);

  signals:
    void spent(const QString& machine, const QString& cli, const QString& email,
               const QString& title, const QString& why);
    void declined(const QString& machine, const QString& cli, const QString& reason);
    void uncertain(const QString& machine, const QString& cli, const QString& reason);

  private:
    enum class Phase : std::uint8_t { prepare, consume, reconcile };
    enum class AfterWrite : std::uint8_t { admitted, cancelled, completed };
    struct Run;
    void run(AgentTarget target, bool asked);
    bool loadState(const std::shared_ptr<Run>& run);
    bool loadAccountState(const std::shared_ptr<Run>& run, const QString& account_id);
    void readCredentials(const std::shared_ptr<Run>& run);
    void start(const std::shared_ptr<Run>& run);
    void finish(const std::shared_ptr<Run>& run);
    void prepared(const std::shared_ptr<Run>& run, const QJsonObject& account);
    void completed(const std::shared_ptr<Run>& run, const QJsonObject& account);
    void persist(const std::shared_ptr<Run>& run, const QJsonObject& state, AfterWrite next);
    void persisted(const std::shared_ptr<Run>& run, AfterWrite next, const QJsonObject& previous);
    void refuse(const std::shared_ptr<Run>& run, const QString& reason);
    [[nodiscard]] QStringList arguments(const Run& run) const;
    [[nodiscard]] bool current(const std::shared_ptr<Run>& run) const;
    Agents agents_;
    Agent agent_;
    Program program_;
    Credentials credentials_;
    QString script_path_;
    QString state_folder_;
    bool helper_ready_{};
    LimitResetSettings settings_;
    QTimer timer_;
    QTimer first_sweep_;
    QHash<QString, std::shared_ptr<Run>> running_;
    QSet<QString> uncertain_notified_;
};
} // namespace lapis::desktop
#endif
