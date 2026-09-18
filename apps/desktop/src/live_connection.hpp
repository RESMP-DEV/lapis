#ifndef LAPIS_DESKTOP_LIVE_CONNECTION_HPP
#define LAPIS_DESKTOP_LIVE_CONNECTION_HPP
#include "transport/local_protocol.hpp"
#include "workspace.hpp"
#include <QFutureWatcher>
#include <QLocalSocket>
#include <QTimer>
#include <optional>

namespace lapis::desktop {
class LiveConnection final : public QObject {
  public:
    LiveConnection(SessionPreview& document, QString endpoint, const session::LaunchSpec& launch,
                   session::wire::AttachMode mode);
    ~LiveConnection() override;
    void begin(session::wire::AttachMode mode);
    void send(session::wire::Kind kind, const QByteArray& payload);
    void resize(session::TerminalSize size);
    void setWantedSize(session::TerminalSize size);
    void applyWantedSize();
    void requestHistory(session::wire::HistoryDirection direction, quint64 reference);
    void cancelHistoryRequest();

  private:
    static constexpr int max_attempts = 30;
    void resetSocket();
    void connectSocket();
    void receive();
    void handle(const session::wire::Frame& frame);
    void acceptHello(const session::wire::Hello& hello);
    void acceptSnapshot(session::wire::SnapshotMessage message);
    void acceptHistoryReply(session::wire::HistoryReply reply);
    void rememberCanceledHistoryRequest(quint64 request_id);
    void invalidateHistory();
    void persistIdentity();
    void finishSynchronization();
    void report(const QString& message);
    void fail(const QString& message,
              session::wire::StatusCode code = session::wire::StatusCode::rejected);
    SessionPreview& document_;
    QString endpoint_;
    QStringList service_arguments_;
    QByteArray fingerprint_;
    std::unique_ptr<QLocalSocket> socket_;
    std::unique_ptr<QFutureWatcher<QString>> descriptor_write_;
    QTimer retry_;
    QTimer handshake_;
    QTimer history_timeout_;
    QByteArray buffer_;
    session::wire::AttachRequest request_;
    std::optional<session::wire::Attachment> attachment_;
    quint64 last_sequence_{};
    quint64 next_history_request_id_{1};
    bool history_request_ids_exhausted_{};
    std::optional<quint64> outstanding_history_request_;
    QSet<quint64> canceled_history_requests_;
    int attempts_{};
    bool connected_{};
    bool ready_{};
    bool failed_{true};
    session::TerminalSize wanted_size_{100, 30};
};
} // namespace lapis::desktop
#endif
