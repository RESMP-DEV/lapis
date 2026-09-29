#ifndef LAPIS_DESKTOP_LIVE_CONNECTION_HPP
#define LAPIS_DESKTOP_LIVE_CONNECTION_HPP
#include "transport/local_protocol.hpp"
#include <QFutureWatcher>
#include <QHash>
#include <QLocalSocket>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QTimer>
#include <functional>
#include <memory>
#include <optional>

namespace lapis::session {
class DescriptorStore;
class DescriptorTicket;
struct LaunchSpec;
} // namespace lapis::session

namespace lapis::desktop {
class SessionPreview;

struct ServiceLaunchRequest {
    QString program;
    QStringList arguments;
    QString log;
    // Added to this process's environment for the service and its CLI; never
    // written anywhere (it can hold a credential).
    QHash<QString, QString> environment{};
};
using ServiceLauncher = std::function<bool(const ServiceLaunchRequest&)>;

class LiveConnection final : public QObject {
  public:
    LiveConnection(SessionPreview& document, QString endpoint, const session::LaunchSpec& launch,
                   session::wire::AttachMode mode,
                   std::shared_ptr<session::DescriptorStore> descriptor_store = {},
                   ServiceLauncher launcher = {});
    ~LiveConnection() override;
    void begin(session::wire::AttachMode mode);
    bool send(session::wire::Kind kind, const QByteArray& payload);
    // Text longer than one message, in as many as it needs; all are queued or
    // none is.
    bool sendText(const QByteArray& text);
    void resize(session::TerminalSize size);
    // Takes the size back after another device (a joined phone) resized the
    // agent: sends this view's size again, once per size shown.
    void claimSize();
    void setWantedSize(session::TerminalSize size);
    void applyWantedSize();
    // Sends the wanted size when the terminal has another; a new snapshot
    // permits one retry, while requests without snapshot progress coalesce.
    void sendWantedSize();
    void requestHistory(session::wire::HistoryDirection direction, quint64 reference);
    void cancelHistoryRequest();

  private:
    static constexpr int max_attempts = 30;
    void resetSocket();
    void connectSocket();
    void sendResize(session::TerminalSize size);
    void receive();
    void handle(const session::wire::Frame& frame);
    void queueHistoryRequest(const session::wire::HistoryRequest& request);
    void acceptHello(const session::wire::Hello& hello);
    void acceptSnapshot(session::wire::SnapshotEnvelope message);
    void acceptHistoryReply(session::wire::HistoryReply reply);
    void rememberCanceledHistoryRequest(quint64 request_id);
    void invalidateHistory();
    void persistIdentity();
    void clearDescriptorWrite();
    void finishSynchronization();
    void report(const QString& message);
    void fail(const QString& message,
              session::wire::StatusCode code = session::wire::StatusCode::rejected);
    SessionPreview& document_;
    QString endpoint_;
    QStringList service_arguments_;
    std::shared_ptr<session::DescriptorStore> descriptor_store_;
    ServiceLauncher launcher_;
    QByteArray fingerprint_;
    std::unique_ptr<QLocalSocket> socket_;
    std::unique_ptr<QFutureWatcher<QString>> descriptor_write_;
    std::shared_ptr<session::DescriptorTicket> descriptor_ticket_;
    QTimer retry_;
    QTimer handshake_;
    QTimer history_timeout_;
    QByteArray buffer_;
    session::wire::AttachRequest request_;
    std::optional<session::wire::Attachment> attachment_;
    quint64 last_sequence_{};
    quint64 resize_sent_after_sequence_{};
    quint64 next_history_request_id_{1};
    bool history_request_ids_exhausted_{};
    std::optional<quint64> outstanding_history_request_;
    std::optional<session::wire::HistoryRequest> deferred_history_;
    std::optional<session::TerminalSize> pending_resize_;
    QSet<quint64> canceled_history_requests_;
    int attempts_{};
    bool connected_{};
    bool ready_{};
    bool failed_{true};
    bool capability_retry_{};
    session::TerminalSize wanted_size_{100, 30};
    bool wanted_size_requested_{};
    session::TerminalSize shown_size_{};
    std::optional<session::TerminalSize> claimed_over_;
};
} // namespace lapis::desktop
#endif
