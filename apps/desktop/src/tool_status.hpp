#ifndef LAPIS_DESKTOP_TOOL_STATUS_HPP
#define LAPIS_DESKTOP_TOOL_STATUS_HPP

#include <QDateTime>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantList>

#include <functional>
#include <list>
#include <optional>

namespace lapis::desktop {

// The Tools pane's answer for each known harness. Rows come from the canonical
// harness catalog, which remains the only launch and restore authority; this
// object only asks whether its command resolves and, when configured, what its
// bounded version probe reports. It never changes launch metadata.
class ToolStatus final : public QObject {
    Q_OBJECT
    // One row per catalog harness, in catalog order:
    // [{id, label, command, adapter, update, resumeOption, offered, resolved,
    //   state, exitCode, detail, checked (ISO 8601, or "" before a check),
    //   fresh, probed}]
    // state: ok, failed, timeout, missing, present (resolves, no probe), or
    // unknown (never checked). An old answer keeps its wording and timestamp;
    // fresh=false alone says that it is stale.
    Q_PROPERTY(QVariantList rows READ rows NOTIFY changed)

  public:
    // The installed program for a catalog id, or empty when absent. Production
    // uses harness_program; tests use private executable fixtures.
    using Programs = std::function<QString(const QString&)>;

    struct Probe {
        QStringList argv; // First word is the catalog id in production.
        int timeoutMs{3000};
        int expectedExit{};
    };

    static constexpr int kFreshMs = 5 * 60 * 1000;
    static constexpr int kProbesAtOnce = 3;
    static constexpr qsizetype kLongestOutput = qsizetype{64} * 1024;
    static constexpr int kLongestDetail = 200;

    explicit ToolStatus(Programs programs = {}, QObject* parent = nullptr);
    ToolStatus(const ToolStatus&) = delete;
    ToolStatus& operator=(const ToolStatus&) = delete;

    // Ask every harness that has not answered or whose answer went stale.
    Q_INVOKABLE void refresh();
    // The pane's per-row rerun asks one harness again whatever it said.
    Q_INVOKABLE void refreshTool(const QString& id);
    [[nodiscard]] QVariantList rows() const;

    // Tests: replace one catalog probe and the clock that decides staleness.
    // A probe with empty argv means "resolution only"; nullopt restores the
    // catalog's default version probe.
    void setProbeForTesting(const QString& id, std::optional<Probe> probe);
    void setNowForTesting(QDateTime now) { now_ = std::move(now); }
    // Tests: republish rows immediately after moving the test clock, instead
    // of waiting for the production stale scan.
    void publishForTesting() { emit changed(); }

  signals:
    void changed();

  private:
    struct Result {
        QString resolved;
        QString state = QStringLiteral("unknown");
        int exitCode{};
        QString detail;
        QDateTime checked;
    };
    struct Running {
        QString id;
        QPointer<QProcess> process;
        QPointer<QTimer> timeout;
        bool timedOut{};
    };

    [[nodiscard]] std::optional<Probe> probeFor(const QString& id) const;
    void queue(const QString& id);
    void startNext();
    void finished(QProcess* process);
    void note(const QString& id, Result result);
    [[nodiscard]] QDateTime now() const;
    [[nodiscard]] bool fresh(const Result& result) const;

    Programs programs_;
    QHash<QString, Result> results_;
    QHash<QString, bool> fresh_noted_;
    QHash<QString, Probe> test_probes_;
    std::list<Running> running_;
    QStringList pending_;
    QTimer stale_check_;
    QDateTime now_;
};

} // namespace lapis::desktop

#endif
