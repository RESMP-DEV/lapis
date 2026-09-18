#ifndef LAPIS_DESKTOP_LIVE_CONNECTION_HPP
#define LAPIS_DESKTOP_LIVE_CONNECTION_HPP
#include "transport/local_protocol.hpp"
#include "workspace.hpp"
#include <QLocalSocket>
#include <QTimer>

namespace lapis::desktop {
class LiveConnection final : public QObject {
  public:
    LiveConnection(SessionPreview& document, QString endpoint, const session::LaunchSpec& launch);
    ~LiveConnection() override;
    void send(session::wire::Kind kind, const QByteArray& payload);
    void resize(session::TerminalSize size);

  private:
    static constexpr int max_attempts = 30;
    void connectSocket();
    void receive();
    void report(const QString& message);
    void fail(const QString& message);
    SessionPreview& document_;
    QString endpoint_;
    QStringList service_arguments_;
    QByteArray fingerprint_;
    QLocalSocket socket_;
    QTimer retry_;
    QTimer handshake_;
    QByteArray buffer_;
    int attempts_{};
    bool connected_{};
    bool launched_{};
    bool ready_{};
    bool failed_{};
    session::TerminalSize wanted_size_{100, 30};
};
} // namespace lapis::desktop
#endif
