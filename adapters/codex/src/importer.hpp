#ifndef LAPIS_CODEX_IMPORTER_HPP
#define LAPIS_CODEX_IMPORTER_HPP

#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>
#include <memory>
#include <optional>

namespace lapis::codex {
// One workspace onboarding/import task on a dedicated Codex server. This is
// deliberately independent from Observer and attention state. Every start is
// explicit; transport loss, malformed data and timeout are terminal for the
// task and are never retried automatically.
class Importer final : public QObject {
    Q_OBJECT
  public:
    enum class Phase {
        disconnected,
        connecting,
        ready,
        detecting,
        importing,
        completed,
        completed_with_failures,
        failed,
    };
    Q_ENUM(Phase)
    enum class ItemType {
        agents_md,
        config,
        skills,
        plugins,
        mcp_server_config,
        subagents,
        hooks,
        commands,
        memory,
        sessions,
    };
    Q_ENUM(ItemType)

    struct DetectedSession {
        QString cwd;
        QString path;
        std::optional<QString> title;
    };
    // payload is the server-provided migration item, retained unchanged for
    // the import request. Description is bounded and displayed verbatim.
    struct DetectedItem {
        ItemType item_type{};
        QString description;
        bool home_scoped{};
        QVector<DetectedSession> sessions;
        QJsonObject payload;
    };
    struct Detection {
        QVector<DetectedItem> items;
    };
    struct DetectOptions {
        bool include_home = true;
        QStringList cwds;
        std::optional<quint32> maximum_session_age_days = 30;
        std::optional<quint32> maximum_sessions = 100;
    };
    struct ImportedSession {
        std::optional<QString> cwd;
        QString target;
        std::optional<QString> source;
        std::optional<QString> title;
    };
    struct ImportFailure {
        QString failure_stage;
        QString message;
        std::optional<QString> cwd;
        std::optional<QString> source;
        std::optional<QString> error_type;
        std::optional<QString> sub_error_type;
    };
    struct ItemResult {
        ItemType item_type{};
        QVector<ImportedSession> sessions;
        QVector<ImportFailure> failures;
    };
    struct Completion {
        QString import_id;
        QVector<ItemResult> results;
    };

    explicit Importer(QObject* parent = nullptr);
    ~Importer() override;
    void start(const QString& socket);
    void stop(); // Explicit stop clears this import task's volatile state.
    // The first slice requests Claude detection only. Selection at import time
    // is further restricted to explicit SESSIONS migration items.
    [[nodiscard]] bool detect();
    [[nodiscard]] bool detect(const DetectOptions& options);
    // Select detected migration items by position. Every index must identify a
    // nonempty SESSIONS item; the selected payload is submitted unchanged.
    [[nodiscard]] bool importSessions(const QVector<qsizetype>& item_positions);
    [[nodiscard]] Phase phase() const;
    [[nodiscard]] const QString& diagnostic() const;
    [[nodiscard]] const Detection& detection() const;
    [[nodiscard]] const QVector<ItemResult>& progress() const;
    [[nodiscard]] const Completion& completion() const;
    [[nodiscard]] QString importId() const;
    [[nodiscard]] QVector<ImportedSession> importedSessions() const;

  signals:
    void connected(); // Initialized; detect() is now allowed.
    void detected();
    void importAccepted(const QString& import_id);
    void importProgress();
    void importCompleted();
    void changed();
    void failed(const QString& reason);

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace lapis::codex
#endif
