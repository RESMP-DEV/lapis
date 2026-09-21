#ifndef LAPIS_CODEX_UNIX_WEBSOCKET_HPP
#define LAPIS_CODEX_UNIX_WEBSOCKET_HPP

#include <QByteArray>
#include <QObject>
#include <QString>
#include <memory>

namespace lapis::codex {
// Single-thread asynchronous text transport for the Codex private Unix endpoint.
// No automatic reconnect or retransmission. All signals run on the owning thread.
class UnixWebSocket final : public QObject {
    Q_OBJECT
  public:
    static constexpr qsizetype maximum_message_bytes = qsizetype{1024} * 1024;
    explicit UnixWebSocket(QObject* parent = nullptr);
    ~UnixWebSocket() override;
    void open(const QString& path);
    void close(); // Explicit close is silent and cancels outstanding work.
    // False means the whole message was not accepted; failed() reports transport loss.
    [[nodiscard]] bool send(const QByteArray& text);
  signals:
    void opened();
    void message(const QByteArray& text);
    void failed(const QString& reason);

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace lapis::codex
#endif
