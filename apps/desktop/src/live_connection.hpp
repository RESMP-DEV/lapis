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

  private:
    static constexpr int max_attempts = 30;
    void resetSocket();
    void connectSocket();
    void receive();
    void handle(const session::wire::Frame& frame);
    void acceptHello(const session::wire::Hello& hello);
    void acceptSnapshot(session::wire::SnapshotMessage message);
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
    QByteArray buffer_;
    session::wire::AttachRequest request_;
    std::optional<session::wire::Attachment> attachment_;
    quint64 last_sequence_{};
    int attempts_{};
    bool connected_{};
    bool ready_{};
    bool failed_{true};
    session::TerminalSize wanted_size_{100, 30};
};
} // namespace lapis::desktop
#endif
