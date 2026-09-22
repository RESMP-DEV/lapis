#ifndef LAPIS_DESKTOP_WORKSPACE_HPP
#define LAPIS_DESKTOP_WORKSPACE_HPP

#include <lapis/session/terminal.hpp>

#include "launch_spec.hpp"
#include "transport/attention_protocol.hpp"
#include "transport/local_protocol.hpp"
#include "workspace_registry.hpp"

#include <QColor>
#include <QMap>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QVariantList>

#include <memory>
#include <optional>
#include <vector>

namespace lapis::desktop {

class LiveConnection;

class SessionPreview final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString sessionId READ sessionId NOTIFY connectionChanged)
    Q_PROPERTY(bool attentionPending READ attentionPending NOTIFY attentionChanged)
    Q_PROPERTY(QString attentionReason READ attentionReason NOTIFY attentionChanged)
    Q_PROPERTY(quint32 attentionSerial READ attentionSerial NOTIFY attentionChanged)
    Q_PROPERTY(int attentionCount READ attentionCount NOTIFY attentionChanged)
    Q_PROPERTY(bool hasAttentionSource READ hasAttentionSource NOTIFY attentionChanged)
    Q_PROPERTY(bool attentionReady READ attentionReady NOTIFY attentionChanged)
    Q_PROPERTY(QString attentionDiagnostic READ attentionDiagnostic NOTIFY attentionChanged)
    Q_PROPERTY(QVariantList attentionRequests READ attentionRequests NOTIFY attentionChanged)
    Q_PROPERTY(QString title READ title CONSTANT)
    Q_PROPERTY(QString directory READ directory CONSTANT)
    Q_PROPERTY(QString activity READ activity NOTIFY snapshotChanged)
    Q_PROPERTY(bool live READ live CONSTANT)
    Q_PROPERTY(bool inputReady READ inputReady NOTIFY connectionChanged)
    Q_PROPERTY(QString connectionState READ connectionState NOTIFY connectionChanged)
    Q_PROPERTY(QString serviceSessionId READ serviceSessionId NOTIFY connectionChanged)
    Q_PROPERTY(QVariantMap snapshotTiming READ snapshotTiming NOTIFY snapshotChanged)
    Q_PROPERTY(bool historyActive READ historyActive NOTIFY historyChanged)
    Q_PROPERTY(bool historyRequestPending READ historyRequestPending NOTIFY historyChanged)
    Q_PROPERTY(QString historyMessage READ historyMessage NOTIFY historyChanged)
    Q_PROPERTY(QColor accent READ accent CONSTANT)
  public:
    SessionPreview(QString title, QString directory, QString activity, QColor accent,
                   std::string_view content);
    ~SessionPreview() override;
    void startLive(const QString& endpoint, const session::LaunchSpec& launch,
                   session::wire::AttachMode mode = session::wire::AttachMode::reconnect);
    void restoreLive(const WorkspaceEntry& entry);
    [[nodiscard]] std::optional<WorkspaceEntry> reconnectEntry() const;
    Q_INVOKABLE void reconnect();
    Q_INVOKABLE void discoverSession();
    Q_INVOKABLE void startNewSession();
    Q_INVOKABLE void olderHistory();
    Q_INVOKABLE void newerHistory();
    Q_INVOKABLE void returnToLive();
    Q_INVOKABLE bool respondAttention(const QString& token, const QVariantMap& response);
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
    void setSessionId(const QString& id) {
        if (session_id_ != id) {
            session_id_ = id;
            emit connectionChanged();
        }
    }
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
    void connectionChanged();
    void snapshotChanged();
    void historyChanged();
    void attentionChanged();
    void attentionArrived();

  private:
    std::unique_ptr<LiveConnection> live_;
    QString session_id_;
    QMap<QString, QString> requests_;
    std::optional<session::wire::AttentionSnapshot> attention_;
    QSet<QString> submitted_attention_;
    quint32 attention_serial_{};
    bool live_snapshot_ready_{};
    bool live_snapshot_received_{};
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
    QString manifest{}; // Empty retains the explicit single-session connection path.
};

class Workspace final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList sessions READ sessions NOTIFY sessionsChanged)
    Q_PROPERTY(bool registryEnabled READ registryEnabled CONSTANT)
    Q_PROPERTY(bool loading READ loading NOTIFY workspaceChanged)
    Q_PROPERTY(bool canAddSessions READ canAddSessions NOTIFY workspaceChanged)
    Q_PROPERTY(QString status READ status NOTIFY workspaceChanged)
    Q_PROPERTY(bool previewMode READ previewMode CONSTANT)
    Q_PROPERTY(int focusedIndex READ focusedIndex WRITE setFocusedIndex NOTIFY focusChanged)
    Q_PROPERTY(
        lapis::desktop::SessionPreview* focusedSession READ focusedSession NOTIFY focusChanged)
  public:
    explicit Workspace(WorkspaceMode mode = WorkspaceMode::live, WorkspaceOptions options = {});
    ~Workspace() override;
    [[nodiscard]] bool registryEnabled() const { return !manifest_.isEmpty(); }
    [[nodiscard]] bool loading() const { return loading_; }
    [[nodiscard]] bool canAddSessions() const;
    [[nodiscard]] const QString& status() const { return status_; }
    Q_INVOKABLE bool addSession(bool codex, const QString& directory, const QString& endpoint = {});
    Q_INVOKABLE bool removeSession(const QString& id);
    Q_INVOKABLE void setInteractionBlocked(const QString& reason, bool blocked);
    [[nodiscard]] bool previewMode() const { return preview_mode_; }
    [[nodiscard]] SessionPreview* session(const QString& id) const;
    // Development fixture v1 only. No calls are accepted in a live workspace.
    bool requestAttention(const PreviewRequest& request);
    bool resolveAttention(const PreviewRequest& request);
    Q_INVOKABLE bool replayAttention(const QString& scenario);
    // Manual traversal of the flat session list, including labelled fixtures.
    // Category grouping is not implemented.
    Q_INVOKABLE void nextSession(int delta = 1);
    [[nodiscard]] QVariantList sessions() const;
    [[nodiscard]] int focusedIndex() const { return focused_index_; }
    [[nodiscard]] SessionPreview* focusedSession() const;
    void setFocusedIndex(int index);
  signals:
    void focusChanged();
    void sessionsChanged();
    void workspaceChanged();

  private:
    [[nodiscard]] static QString rootDirectory();
    [[nodiscard]] static QString defaultEndpoint();
    class Storage;
    std::unique_ptr<Storage> storage_;
    void loadRegistry();
    void persistRegistry();
    void appendSession(std::unique_ptr<SessionPreview> session);
    void flushFocus();
    QString manifest_;
    QString status_;
    QPointer<SessionPreview> pending_focus_;
    QSet<QString> interaction_blocks_;
    bool loading_{};
    bool registry_ready_{};
    std::vector<std::unique_ptr<SessionPreview>> sessions_;
    int focused_index_{};
    bool preview_mode_{};
};

} // namespace lapis::desktop
#endif // LAPIS_DESKTOP_WORKSPACE_HPP
