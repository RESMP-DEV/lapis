#ifndef LAPIS_DESKTOP_WORKSPACE_HPP
#define LAPIS_DESKTOP_WORKSPACE_HPP

#include <lapis/session/terminal.hpp>

#include <QColor>
#include <QMap>
#include <QObject>
#include <QString>
#include <QVariantList>

#include <memory>
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
    Q_PROPERTY(QString title READ title CONSTANT)
    Q_PROPERTY(QString directory READ directory CONSTANT)
    Q_PROPERTY(QString activity READ activity NOTIFY snapshotChanged)
    Q_PROPERTY(bool live READ live CONSTANT)
    Q_PROPERTY(QColor accent READ accent CONSTANT)
  public:
    SessionPreview(QString title, QString directory, QString activity, QColor accent,
                   std::string_view content);
    ~SessionPreview() override;
    void startLive(const QString& endpoint, const QString& directory);
    void applySnapshot(session::TerminalSnapshot snapshot);
    void setActivity(const QString& activity);
    void sendText(const QByteArray& bytes, bool paste = false);
    void sendKey(session::TerminalKey key, session::KeyModifiers modifiers);
    void resizeTerminal(session::TerminalSize size);
    void setSessionId(const QString& id) { session_id_ = id; }
    [[nodiscard]] const QString& sessionId() const { return session_id_; }
    [[nodiscard]] bool attentionPending() const { return !requests_.isEmpty(); }
    [[nodiscard]] QString attentionReason() const {
        return requests_.isEmpty() ? QString{} : requests_.first();
    }
    [[nodiscard]] quint32 attentionSerial() const { return attention_serial_; }
    [[nodiscard]] int attentionCount() const { return static_cast<int>(requests_.size()); }
    bool addPreviewRequest(const QString& id, const QString& reason);
    bool resolvePreviewRequest(const QString& id);
    void clearPreviewRequests();
    [[nodiscard]] bool liveSnapshotReady() const { return live_snapshot_ready_; }
    [[nodiscard]] bool live() const { return live_ != nullptr; }
    [[nodiscard]] const QString& title() const { return title_; }
    [[nodiscard]] const QString& directory() const { return directory_; }
    [[nodiscard]] const QString& activity() const { return activity_; }
    [[nodiscard]] QColor accent() const { return accent_; }
    [[nodiscard]] const session::TerminalSnapshot& snapshot() const { return snapshot_; }

  signals:
    void snapshotChanged();
    void attentionChanged();
    void attentionArrived();

  private:
    std::unique_ptr<LiveConnection> live_;
    QString session_id_;
    QMap<QString, QString> requests_;
    quint32 attention_serial_{};
    bool live_snapshot_ready_{};
    QString title_;
    QString directory_;
    QString activity_;
    QColor accent_;
    session::TerminalSnapshot snapshot_;
};

enum class WorkspaceMode : std::uint8_t { live, preview };

struct PreviewRequest {
    QString session_id;
    QString request_id;
    QString reason;
};

class Workspace final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList sessions READ sessions CONSTANT)
    Q_PROPERTY(bool previewMode READ previewMode CONSTANT)
    Q_PROPERTY(int focusedIndex READ focusedIndex WRITE setFocusedIndex NOTIFY focusChanged)
    Q_PROPERTY(
        lapis::desktop::SessionPreview* focusedSession READ focusedSession NOTIFY focusChanged)
  public:
    explicit Workspace(WorkspaceMode mode = WorkspaceMode::live);
    [[nodiscard]] bool previewMode() const { return preview_mode_; }
    [[nodiscard]] SessionPreview* session(const QString& id) const;
    // Development fixture v1 only. No calls are accepted in a live workspace.
    bool requestAttention(const PreviewRequest& request);
    bool resolveAttention(const PreviewRequest& request);
    Q_INVOKABLE bool replayAttention(const QString& scenario);
    [[nodiscard]] QVariantList sessions() const;
    [[nodiscard]] int focusedIndex() const { return focused_index_; }
    [[nodiscard]] SessionPreview* focusedSession() const;
    void setFocusedIndex(int index);
  signals:
    void focusChanged();

  private:
    std::vector<std::unique_ptr<SessionPreview>> sessions_;
    int focused_index_{};
    bool preview_mode_{};
};

} // namespace lapis::desktop
#endif // LAPIS_DESKTOP_WORKSPACE_HPP
