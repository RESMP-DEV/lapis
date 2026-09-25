#ifndef LAPIS_DESKTOP_HARNESS_MODELS_HPP
#define LAPIS_DESKTOP_HARNESS_MODELS_HPP
#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <QThreadPool>
#include <QTimer>
#include <functional>
#include <vector>

namespace lapis::desktop {

// A model a CLI can start with.
struct ModelChoice {
    QString id{};   // given with the CLI's model flag
    QString name{}; // as the CLI names it
    // The CLI's own default: an agent on it starts without the flag.
    bool isDefault{};
    bool operator==(const ModelChoice&) const = default;
};

// What each CLI says it can use, with its default first and the rest in the
// CLI's order, or most recently used first where the CLI keeps a record.
[[nodiscard]] std::vector<ModelChoice> codex_models(const QJsonObject& result);
[[nodiscard]] std::vector<ModelChoice> claude_models(const QJsonObject& initialized);
[[nodiscard]] std::vector<ModelChoice> grok_models(const QString& listing);
[[nodiscard]] std::vector<ModelChoice> agy_models(const QString& listing);
[[nodiscard]] std::vector<ModelChoice> kimi_models(const QString& config);
[[nodiscard]] std::vector<ModelChoice> opencode_models(const QJsonObject& state);
[[nodiscard]] std::vector<ModelChoice> omp_models(const QString& config, const QStringList& recent);

// The models each installed CLI offers the person, found in the background
// and refreshed hourly, so the new-agent forms show what their plans have:
//   codex     app-server model/list
//   claude    the initialize control request's model list (no settings, no
//             transcript)
//   grok      `grok models`;  agy  `agy models`
//   kimi      the model aliases in its config.toml
//   opencode  its recent and favourite models
//   omp       its configured roles, then the models its newest sessions used
// At most kMostShown per CLI.
class HarnessModels final : public QObject {
    Q_OBJECT
  public:
    // The CLI program for an id, or empty when it is not installed.
    using Programs = std::function<QString(const QString&)>;
    static constexpr int kFirstMs = 8 * 1000;
    static constexpr int kRefreshMs = 60 * 60 * 1000;
    static constexpr int kAnswerMs = 30 * 1000;
    static constexpr int kMostShown = 8;

    HarnessModels(Programs programs, QString home, QObject* parent = nullptr);
    ~HarnessModels() override;
    HarnessModels(const HarnessModels&) = delete;
    HarnessModels& operator=(const HarnessModels&) = delete;

    // The first refresh soon, then hourly.
    void start();
    Q_INVOKABLE void refresh();
    [[nodiscard]] std::vector<ModelChoice> models(const QString& harness) const;

  signals:
    void changed();

  private:
    struct Asking {
        QString harness;
        QPointer<QProcess> process;
        QByteArray output;
    };
    void ask(const QString& harness, const QStringList& arguments, const QByteArray& input);
    void answered(const QString& harness);
    void readFiles();
    void set(const QString& harness, std::vector<ModelChoice> models);

    Programs programs_;
    QString home_;
    QTimer timer_;
    QThreadPool pool_;
    std::vector<Asking> asking_;
    QHash<QString, std::vector<ModelChoice>> models_;
};
} // namespace lapis::desktop
#endif
