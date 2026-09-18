#ifndef LAPIS_DESKTOP_LIVE_CONNECTION_HPP
#define LAPIS_DESKTOP_LIVE_CONNECTION_HPP
#include "transport/local_protocol.hpp"
#include "workspace.hpp"
#include <QDir>
#include <QLocalSocket>
#include <QTimer>

namespace lapis::desktop {
class LiveConnection final : public QObject {
  public:
    LiveConnection(SessionPreview& document, QString endpoint, const QDir& directory);
    ~LiveConnection() override;
    void send(session::wire::Kind kind, const QByteArray& payload);
    void resize(session::TerminalSize size);

  private:
    void connectSocket();
    void receive();
    void report(const QString& message);
    SessionPreview& document_;
    QString endpoint_;
    QString directory_;
    QLocalSocket socket_;
    QTimer retry_;
    QByteArray buffer_;
    int attempts_{};
    bool launched_{};
    bool ready_{};
    session::TerminalSize wanted_size_{100, 30};
};
} // namespace lapis::desktop
#endif
