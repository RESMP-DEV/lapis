#ifndef LAPIS_DESKTOP_WORKSPACE_HPP
#define LAPIS_DESKTOP_WORKSPACE_HPP

#include <lapis/session/terminal.hpp>

#include "launch_spec.hpp"
#include "transport/attention_protocol.hpp"
#include "transport/local_protocol.hpp"

#include <QColor>
#include <QHash>
#include <QJsonArray>
#include <QJsonValue>
#include <QLockFile>
#include <QMap>
#include <QObject>
#include <QSet>
#include <QString>
#include <QTimer>
#include <QVariantList>

#include <atomic>
#include <memory>
#include <optional>
#include <vector>

namespace lapis::desktop {

class LiveConnection;

class SessionPreview final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString sessionId READ sessionId CONSTANT)
    Q_PROPERTY(bool attentionPending READ attentionPending NOTIFY attentionChanged)
    Q_PROPERTY(QString attentionReason READ attentionReason NOTIFY attentionChanged)
    Q_PROPERTY(quint32 attentionSerial READ attentionSerial NOTIFY attentionChanged)
    Q_PROPERTY(int attentionCount READ attentionCount NOTIFY attentionChanged)
    Q_PROPERTY(bool hasAttentionSource READ hasAttentionSource NOTIFY attentionChanged)
    Q_PROPERTY(bool attentionReady READ attentionReady NOTIFY attentionChanged)
    Q_PROPERTY(QString attentionDiagnostic READ attentionDiagnostic NOTIFY attentionChanged)
    Q_PROPERTY(QVariantList attentionRequests READ attentionRequests NOTIFY attentionChanged)
    Q_PROPERTY(QString title READ title NOTIFY identityChanged)
    Q_PROPERTY(QString statusLabel READ statusLabel NOTIFY statusChanged)
    Q_PROPERTY(QString statusKind READ statusKind NOTIFY statusChanged)
    Q_PROPERTY(QString agentName READ agentName NOTIFY identityChanged)
    Q_PROPERTY(QString harnessId READ harnessId NOTIFY identityChanged)
    Q_PROPERTY(QString directory READ directory CONSTANT)
    Q_PROPERTY(QString activity READ activity NOTIFY snapshotChanged)
    Q_PROPERTY(bool live READ live CONSTANT)
    Q_PROPERTY(bool inputReady READ inputReady NOTIFY connectionChanged)
    Q_PROPERTY(bool reachable READ reachable NOTIFY connectionChanged)
    Q_PROPERTY(QString connectionState READ connectionState NOTIFY connectionChanged)
    Q_PROPERTY(QString serviceSessionId READ serviceSessionId NOTIFY connectionChanged)
    Q_PROPERTY(QVariantMap snapshotTiming READ snapshotTiming NOTIFY snapshotChanged)
    Q_PROPERTY(bool historyActive READ historyActive NOTIFY historyChanged)
    Q_PROPERTY(bool historyRequestPending READ historyRequestPending NOTIFY historyChanged)
    Q_PROPERTY(QString historyMessage READ historyMessage NOTIFY historyChanged)
    Q_PROPERTY(QColor accent READ accent CONSTANT)
    // Set between an explicit close request and the process ending.
    Q_PROPERTY(bool closing READ closing NOTIFY statusChanged)
    // Finished a turn or started needing a response while another agent was
    // selected; cleared when this agent is selected.
    Q_PROPERTY(bool unseen READ unseen NOTIFY unseenChanged)
  public:
    SessionPreview(QString title, QString directory, QString activity, QColor accent,
                   std::string_view content);
    ~SessionPreview() override;
    void startLive(const QString& endpoint, const session::LaunchSpec& launch,
                   session::wire::AttachMode mode = session::wire::AttachMode::reconnect);
    Q_INVOKABLE void reconnect();
    Q_INVOKABLE void discoverSession();
    Q_INVOKABLE void startNewSession();
    Q_INVOKABLE void olderHistory();
    Q_INVOKABLE void newerHistory();
    Q_INVOKABLE void returnToLive();
    Q_INVOKABLE bool respondAttention(const QString& token, const QVariantMap& response);
    // Ask the session service to end this agent's process. Returns false when
    // the request could not be queued on a synchronized connection.
    bool terminate();
    [[nodiscard]] bool closing() const { return closing_; }
    void setClosing(bool closing);
    [[nodiscard]] bool unseen() const { return unseen_; }
    void setUnseen(bool unseen);
    // Where activity comes from: a service-side observer (the Codex app-server,
    // Claude Code's hook relay) or, for other CLIs, an output-timing estimate.
    enum class StatusSource : std::uint8_t { observer, output };
    void setStatusSource(StatusSource source) { status_source_ = source; }
    [[nodiscard]] StatusSource statusSource() const { return status_source_; }
    // Output estimate windows: ignore replays for `settle_ms` after attaching,
    // treat three frames within `burst_ms` as activity, and `quiet_ms` of
    // silence after it as a pause. Tests shorten them.
    struct OutputTiming {
        qint64 settle_ms{3000};
        qint64 burst_ms{1500};
        int quiet_ms{4000};
    };
    void setOutputTimingForTesting(const OutputTiming& timing) { output_timing_ = timing; }
    void applyAttention(session::wire::AttentionSnapshot snapshot);
    void invalidateAttention();
    void retryAttention(const session::wire::AttentionDecision& decision);
    [[nodiscard]] bool hasAttentionSource() const;
    [[nodiscard]] bool attentionReady() const;
    [[nodiscard]] QString attentionDiagnostic() const;
    [[nodiscard]] QVariantList attentionRequests() const;
    void setConnection(const QString& state, bool input_ready);
    void setServiceIdentity(const QByteArray& identity);
    [[nodiscard]] bool inputReady() const {
        return input_ready_ && !history_active_ && !history_request_pending_;
    }
    // Synchronized with its service, even while a history page is showing.
    [[nodiscard]] bool reachable() const { return input_ready_; }
    [[nodiscard]] const QString& connectionState() const { return connection_state_; }
    [[nodiscard]] const QString& serviceSessionId() const { return service_session_id_; }
    void applySnapshot(session::TerminalSnapshot snapshot);
    void setSnapshotTiming(const QVariantMap& timing) { snapshot_timing_ = timing; }
    [[nodiscard]] QVariantMap snapshotTiming() const { return snapshot_timing_; }
    void beginHistoryRequest();
    void completeHistoryRequest(quint64 page_id, session::TerminalSnapshot snapshot,
                                const QString& message);
    void failHistoryRequest(const QString& message);
    void cancelHistoryRequests();
    void setHistoryRequestId(quint64 request_id);
    void setActivity(const QString& activity);
    void sendText(const QByteArray& bytes, bool paste = false);
    void sendKey(session::TerminalKey key, session::KeyModifiers modifiers);
    void resizeTerminal(session::TerminalSize size);
    [[nodiscard]] QString statusLabel() const;
    [[nodiscard]] QString statusKind() const;
    [[nodiscard]] QString agentName() const;
    [[nodiscard]] const QString& harnessId() const { return harness_id_; }
    void setHarnessId(const QString& id);
    void rename(const QString& title) {
        title_ = title;
        emit identityChanged();
    }
    void setSessionId(const QString& id) { session_id_ = id; }
    [[nodiscard]] const QString& sessionId() const { return session_id_; }
    [[nodiscard]] bool attentionPending() const { return attentionCount() != 0; }
    [[nodiscard]] QString attentionReason() const;
    [[nodiscard]] quint32 attentionSerial() const { return attention_serial_; }
    [[nodiscard]] int attentionCount() const;
    bool addPreviewRequest(const QString& id, const QString& reason);
    bool resolvePreviewRequest(const QString& id);
    void clearPreviewRequests();
    [[nodiscard]] bool liveSnapshotReady() const { return live_snapshot_ready_; }
    [[nodiscard]] bool historyActive() const { return history_active_; }
    [[nodiscard]] bool historyRequestPending() const { return history_request_pending_; }
    [[nodiscard]] const QString& historyMessage() const { return history_message_; }
    [[nodiscard]] bool live() const { return live_ != nullptr; }
    [[nodiscard]] const QString& title() const { return title_; }
    [[nodiscard]] const QString& directory() const { return directory_; }
    [[nodiscard]] const QString& activity() const { return activity_; }
    [[nodiscard]] QColor accent() const { return accent_; }
    [[nodiscard]] const session::TerminalSnapshot& snapshot() const { return snapshot_; }

  signals:
    void identityChanged();
    void statusChanged();
    void connectionChanged();
    void snapshotChanged();
    void historyChanged();
    void attentionChanged();
    void attentionArrived();
    void unseenChanged();

  private:
    std::unique_ptr<LiveConnection> live_;
    QString harness_id_{QStringLiteral("codex")};
    QString session_id_;
    QMap<QString, QString> requests_;
    std::optional<session::wire::AttentionSnapshot> attention_;
    QSet<QString> submitted_attention_;
    quint32 attention_serial_{};
    bool awaiting_first_prompt_{};
    bool live_snapshot_ready_{};
    bool live_snapshot_received_{};
    bool closing_{};
    bool unseen_{};
    StatusSource status_source_{StatusSource::observer};
    // Output estimate: several frames close together read as activity, and a
    // few quiet seconds after that as a pause. Neither implies a finished task.
    void noteOutput();
    [[nodiscard]] QString unobservedStatusKind() const;
    std::vector<qint64> output_times_;
    bool output_active_{};
    bool output_quiet_{};
    qint64 ready_since_{};
    OutputTiming output_timing_;
    QTimer quiet_timer_;
    bool history_active_{};
    bool history_request_pending_{};
    quint64 history_page_id_{};
    bool input_ready_{};
    QString connection_state_{QStringLiteral("disconnected")};
    QString service_session_id_;
    QString history_message_;
    QVariantMap snapshot_timing_;
    QString title_;
    QString directory_;
    QString activity_;
    QColor accent_;
    session::TerminalSnapshot live_snapshot_;
    session::TerminalSnapshot snapshot_;
};

enum class WorkspaceMode : std::uint8_t { live, preview };

struct PreviewRequest {
    QString session_id;
    QString request_id;
    QString reason;
};

struct WorkspaceOptions {
    QString endpoint;
    std::optional<session::LaunchSpec> launch;
    session::wire::AttachMode mode{session::wire::AttachMode::reconnect};
    QString storagePath{};
    // Restart agents whose session service is gone (after a reboot or crash),
    // resuming each recorded conversation. Off unless the app asks for it.
    bool restoreAgents{};
};

class Workspace final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList sessions READ sessions NOTIFY sessionsChanged)
    Q_PROPERTY(QVariantList categories READ categories NOTIFY categoriesChanged)
    // Agents in any category that finished unseen or wait on a request.
    Q_PROPERTY(int attentionAgents READ attentionAgents NOTIFY categoriesChanged)
    Q_PROPERTY(QVariantList categorySessions READ categorySessions NOTIFY sessionsChanged)
    Q_PROPERTY(QString activeCategoryId READ activeCategoryId NOTIFY categoryChanged)
    Q_PROPERTY(QString workspaceError READ workspaceError NOTIFY errorChanged)
    Q_PROPERTY(bool previewMode READ previewMode CONSTANT)
    Q_PROPERTY(QString homeDirectory READ homeDirectory CONSTANT)
    Q_PROPERTY(int focusedIndex READ focusedIndex NOTIFY focusChanged)
    Q_PROPERTY(
        lapis::desktop::SessionPreview* focusedSession READ focusedSession NOTIFY focusChanged)
  public:
    explicit Workspace(WorkspaceMode mode = WorkspaceMode::live, WorkspaceOptions options = {});
    [[nodiscard]] bool previewMode() const { return preview_mode_; }
    [[nodiscard]] QString homeDirectory() const;
    Q_INVOKABLE [[nodiscard]] QVariantList availableHarnesses() const;
    Q_INVOKABLE [[nodiscard]] QString displayPath(const QString& directory) const;
    [[nodiscard]] SessionPreview* session(const QString& id) const;
    // Development fixture v1 only. No calls are accepted in a live workspace.
    bool requestAttention(const PreviewRequest& request);
    bool resolveAttention(const PreviewRequest& request);
    Q_INVOKABLE bool replayAttention(const QString& scenario);
    [[nodiscard]] QVariantList categories() const;
    [[nodiscard]] QVariantList categorySessions() const;
    [[nodiscard]] const QString& activeCategoryId() const { return active_category_; }
    [[nodiscard]] const QString& workspaceError() const { return error_; }
    Q_INVOKABLE void clearError();
    Q_INVOKABLE bool addCategory(const QString& name);
    Q_INVOKABLE bool renameCategory(const QString& id, const QString& name);
    Q_INVOKABLE bool removeCategory(const QString& id);
    Q_INVOKABLE bool selectCategory(const QString& id);
    Q_INVOKABLE void nextCategory(int delta = 1);
    Q_INVOKABLE bool selectSession(const QString& id);
    Q_INVOKABLE bool createAgent(const QString& directory, const QString& title,
                                 const QString& harness = QStringLiteral("codex"));
    // Close an agent's tab. A reachable agent is ended through its
    // session service first and its tab closes once the process exits. An
    // unreachable one keeps its tab unless `abandon` accepts that it may still
    // be running without one.
    Q_INVOKABLE bool closeSession(const QString& id, bool abandon = false);
    Q_INVOKABLE bool restartAgent(const QString& id);
    Q_INVOKABLE bool moveSession(const QString& id, const QString& categoryId);
    Q_INVOKABLE bool renameSession(const QString& id, const QString& title);
    Q_INVOKABLE bool moveSessionBy(const QString& id, int delta);
    Q_INVOKABLE bool removeSession(const QString& id);
    Q_INVOKABLE void nextSession(int delta = 1);
    // Selects the next agent, in any category, with a pending request, or else
    // one that finished while unseen; false when none is waiting.
    Q_INVOKABLE bool nextAttention();
    [[nodiscard]] QVariantList sessions() const;
    [[nodiscard]] int focusedIndex() const { return focused_index_; }
    [[nodiscard]] SessionPreview* focusedSession() const;
    [[nodiscard]] int attentionAgents() const;
    // Arguments from lapis.json added to each new agent of a harness.
    void setHarnessArguments(QHash<QString, QStringList> arguments) {
        harness_arguments_ = std::move(arguments);
    }
  signals:
    void focusChanged();
    // An agent received a new request.
    void requestArrived();
    void sessionsChanged();
    void categoriesChanged();
    void categoryChanged();
    void errorChanged();

  private:
    [[nodiscard]] static QString rootDirectory();
    [[nodiscard]] static QString defaultEndpoint();
    std::vector<std::unique_ptr<SessionPreview>> sessions_;
    QHash<QString, QStringList> harness_arguments_;
    bool restore_agents_{};
    // Records conversations for agents whose services do not.
    QTimer conversation_timer_;
    std::shared_ptr<std::atomic_bool> probing_{std::make_shared<std::atomic_bool>(false)};
    void recordConversations();
    struct Category {
        QString id;
        QString name;
        QString selected;
    };
    struct Agent {
        QString category;
        QString endpoint;
        session::LaunchSpec launch;
        QString harness{QStringLiteral("codex")};
    };
    std::vector<Category> categories_;
    QMap<QString, Agent> agents_;
    QString active_category_{QStringLiteral("general")};
    QString storage_path_;
    std::unique_ptr<QLockFile> registry_lock_;
    QString error_;
    bool storage_failed_{};
    bool fail(const QString& message);
    struct RegistryState {
        std::vector<Category> categories;
        QMap<QString, Agent> agents;
        QString active;
        int focused{-1};
    };
    [[nodiscard]] RegistryState checkpoint() const;
    void rollback(const RegistryState& previous);
    bool mutableRegistry();
    bool commit(const RegistryState& previous);
    bool save(const QString& renamedId = {}, const QString& renamedTitle = {});
    void restore();
    void loadCategories(const QJsonArray& groups);
    void loadAgents(const QJsonArray& agents);
    void finishClosing(SessionPreview* item);
    [[nodiscard]] static SessionPreview::StatusSource statusSource(const Agent& agent);
    [[nodiscard]] static QStringList savedArguments(const QJsonValue& value);
    [[nodiscard]] static std::optional<session::LaunchSpec>
    restoredLaunch(const Agent& agent, QString* diagnostic = nullptr);
    [[nodiscard]] static bool serviceRunning(const QString& endpoint);
    void noteStatus(SessionPreview* item);
    QHash<const SessionPreview*, QString> last_kind_;
    bool batching_categories_{};
    bool discardSession(const QString& id);
    void changed();
    void restoreSelection();
    void watch(SessionPreview* item);
    int focused_index_{-1};
    bool preview_mode_{};
};

} // namespace lapis::desktop
#endif // LAPIS_DESKTOP_WORKSPACE_HPP
