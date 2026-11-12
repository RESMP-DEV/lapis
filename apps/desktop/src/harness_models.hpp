#ifndef LAPIS_DESKTOP_HARNESS_MODELS_HPP
#define LAPIS_DESKTOP_HARNESS_MODELS_HPP
#include <QByteArray>
#include <QElapsedTimer>
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
#include <functional>
#include <vector>

namespace lapis::desktop {

// A model a CLI can start with.
struct ModelChoice {
    QString id{};   // given with the CLI's model flag
    QString name{}; // as the CLI names it
    // The CLI's own default: an agent on it starts without the flag.
    bool isDefault{};
    // The effort levels the CLI lists for it, in its order (empty: none
    // listed), and the API model an alias stands for ("opus" is
    // "claude-opus-5-5"), as the CLI's listing gives them.
    QStringList efforts{};
    QString resolved{};
    bool operator==(const ModelChoice&) const = default;
};

// One entry of the visual view's model picker. A CLI whose ids join a model
// and an effort ("gemini-4-hydrogen-xhigh" named "Gemini 4 Hydrogen
// (XHigh)") gives one family entry, `levels` holding each level's own id;
// any other model is its own entry with the levels its CLI lists for it.
struct PickerModel {
    QString id;   // the family's id, or the model's
    QString name; // the family's name, or the model's
    QString resolved;
    struct Level {
        QString effort;
        QString id; // the model id that selects this level, or empty
        bool operator==(const Level&) const = default;
    };
    std::vector<Level> levels;
    bool operator==(const PickerModel&) const = default;
};
// Grouped as above, in the listing's order. A level is "-<level>" at the
// end of the id that the name's bracketed level spells ("(Medium)",
// "(XHigh)", "(Extra High)"), so a new family or level groups with no
// change here. Levels are ordered low, medium, high, xhigh, max, then any
// other in the listing's order.
[[nodiscard]] std::vector<PickerModel> picker_models(const std::vector<ModelChoice>& models);
// The same for QML: [{id, name, resolved, levels: [{effort, id}]}].
[[nodiscard]] QVariantList picker_list(const std::vector<PickerModel>& models);

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
// and asked again every ten minutes and when a picker opens on a list older
// than a minute (refreshIfOlder), so the new-agent forms and the visual
// view's picker show what their plans have without a restart. An ask that
// fails (no answer, an empty or unreadable one) keeps the previous list; a
// CLI no longer installed has none:
//   codex     app-server model/list
//   claude    the initialize control request's model list (no settings, no
//             transcript)
//   grok      `grok models`;  agy  `agy models`
//   kimi      the model aliases in its config.toml
//   opencode  its recent and favourite models
//   omp       its configured roles, then the models its newest sessions used
// Every model listed is kept; the new-agent form shows at most kMostShown.
class HarnessModels final : public QObject {
    Q_OBJECT
  public:
    // The CLI program for an id, or empty when it is not installed.
    using Programs = std::function<QString(const QString&)>;
    static constexpr int kFirstMs = 8 * 1000;
    static constexpr int kRefreshMs = 10 * 60 * 1000;
    static constexpr int kStaleMs = 60 * 1000;
    static constexpr int kAnswerMs = 30 * 1000;
    static constexpr int kMostShown = 8;

    HarnessModels(Programs programs, QString home, QObject* parent = nullptr);
    ~HarnessModels() override;
    HarnessModels(const HarnessModels&) = delete;
    HarnessModels& operator=(const HarnessModels&) = delete;

    // The first refresh soon, then every kRefreshMs.
    void start();
    Q_INVOKABLE void refresh();
    // Refreshes when the last refresh began more than `ms` ago (or never);
    // returns whether it did. Never waits for an answer.
    bool refreshIfOlder(int ms = kStaleMs);
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
    QElapsedTimer asked_; // since the last refresh began
    QThreadPool pool_;
    std::vector<Asking> asking_;
    QHash<QString, std::vector<ModelChoice>> models_;
};
} // namespace lapis::desktop
#endif
