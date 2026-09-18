#include "platform/posix/pty_process.hpp"
#include "transport/local_protocol.hpp"

#include <QCoreApplication>
#include <QDataStream>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QPointer>
#include <QTimer>

#include <exception>
#include <stdexcept>

namespace {
using namespace lapis::session;
class SessionService final : public QObject {
  public:
    SessionService(const QString& endpoint, const QDir& directory)
        : lock_(endpoint + QStringLiteral(".lock")), terminal_({100, 30}, limits()) {
        terminal_.feed("\x1b]10;rgb:d9/de/e8\x1b\\\x1b]11;rgb:0d/13/1d\x1b\\");
        lock_.setStaleLockTime(0);
        if (!lock_.tryLock(0))
            throw std::runtime_error("Session service already owns endpoint");
        QLocalServer::removeServer(endpoint);
        server_.setSocketOptions(QLocalServer::UserAccessOption);
        if (!server_.listen(endpoint))
            throw std::runtime_error(server_.errorString().toStdString());
        timer_.setSingleShot(true);
        timer_.setInterval(16);
        connect(&timer_, &QTimer::timeout, this, [this] { publish(); });
        connect(&server_, &QLocalServer::newConnection, this, [this] { attach(); });
        connect(&pty_, &posix::PtyProcess::output, this, [this](const QByteArray& bytes) {
            try {
                terminal_.feed(
                    std::string_view(bytes.constData(), static_cast<std::size_t>(bytes.size())));
                const auto replies = terminal_.take_replies();
                if (!replies.empty() &&
                    !pty_.writeBytes(
                        QByteArray(replies.data(), static_cast<qsizetype>(replies.size()))))
                    throw std::runtime_error("PTY reply queue overflow");
                dirty_ = true;
                schedule();
            } catch (const std::exception& error) {
                stop(QString::fromUtf8(error.what()));
            }
        });
        connect(&pty_, &posix::PtyProcess::failure, this,
                [this](const QString& message) { stop(message); });
        connect(&pty_, &posix::PtyProcess::finished, this,
                [this](int code) { stop(QStringLiteral("Shell exited (%1)").arg(code)); });
        QString shell = qEnvironmentVariable("SHELL");
        if (shell.isEmpty())
            shell = QStringLiteral("/bin/sh");
        pty_.start({.shell = shell, .directory = directory.absolutePath(), .size = {100, 30}});
    }

    ~SessionService() override {
        disconnect(&pty_, nullptr, this, nullptr);
        disconnect(&server_, nullptr, this, nullptr);
        disconnect(&timer_, nullptr, this, nullptr);
        if (client_)
            disconnect(client_, nullptr, this, nullptr);
    }

  private:
    static TerminalLimits limits() {
        TerminalLimits result;
        result.max_cells = wire::max_cells;
        result.max_grapheme_codepoints = wire::max_codepoints;
        result.max_input_bytes = std::size_t{64} * 1024U;
        return result;
    }
    void stop(const QString& message) {
        qInfo().noquote() << message;
        if (client_) {
            client_->write(wire::frame(wire::Kind::status, message.toUtf8()));
            client_->flush();
        }
        QTimer::singleShot(50, QCoreApplication::instance(), &QCoreApplication::quit);
    }
    void attach() {
        auto* incoming = server_.nextPendingConnection();
        if (!incoming)
            return;
        if (client_) {
            client_->disconnectFromServer();
            client_->deleteLater();
        }
        client_ = incoming;
        buffer_.clear();
        incoming->setReadBufferSize(wire::max_frame_bytes + 4);
        connect(incoming, &QLocalSocket::readyRead, this, [this, incoming] {
            if (client_ == incoming)
                receive();
        });
        connect(incoming, &QLocalSocket::bytesWritten, this, [this] { schedule(); });
        connect(incoming, &QLocalSocket::disconnected, incoming, &QObject::deleteLater);
        QByteArray hello;
        QDataStream out(&hello, QIODevice::WriteOnly);
        out << wire::version << quint64(pty_.processId());
        incoming->write(wire::frame(wire::Kind::hello, hello));
        dirty_ = true;
        schedule();
    }
    void schedule() {
        if (client_ && dirty_ && !timer_.isActive())
            timer_.start();
    }
    void publish() {
        if (!client_ || client_->state() != QLocalSocket::ConnectedState || !dirty_)
            return;
        if (client_->bytesToWrite() != 0)
            return; // one replaceable snapshot in flight
        try {
            const auto bytes =
                wire::frame(wire::Kind::snapshot, wire::encode_snapshot(terminal_.snapshot()));
            if (client_->write(bytes) < 0)
                throw std::runtime_error("Session socket write failed");
            dirty_ = false;
        } catch (const std::exception& error) {
            stop(QString::fromUtf8(error.what()));
        }
    }
    void receive() {
        if (!client_ || client_->state() != QLocalSocket::ConnectedState)
            return;
        try {
            buffer_ += client_->readAll();
            if (buffer_.size() > wire::max_frame_bytes + 4)
                throw std::runtime_error("Session input overflow");
            wire::Frame frame;
            for (int processed = 0; processed < 64; ++processed) {
                if (!wire::take_frame(buffer_, frame))
                    return;
                handle(frame);
            }
            const auto attachment = client_;
            QTimer::singleShot(0, this, [this, attachment] {
                if (attachment && client_ == attachment)
                    receive();
            });
        } catch (const std::exception& error) {
            client_->write(wire::frame(wire::Kind::status, QByteArray(error.what())));
            client_->disconnectFromServer();
        }
    }
    void handle(const wire::Frame& frame) {
        if (frame.payload.size() > qsizetype{64} * 1024)
            throw std::runtime_error("Input message too large");
        QByteArray bytes;
        switch (frame.kind) {
        case wire::Kind::text:
            bytes = frame.payload;
            break;
        case wire::Kind::paste: {
            const auto encoded = terminal_.encode_paste(std::string_view(
                frame.payload.constData(), static_cast<std::size_t>(frame.payload.size())));
            bytes = QByteArray(encoded.data(), static_cast<qsizetype>(encoded.size()));
            break;
        }
        case wire::Kind::key: {
            if (frame.payload.size() != 2)
                throw std::runtime_error("Invalid key message");
            const auto key = static_cast<TerminalKey>(static_cast<unsigned char>(frame.payload[0]));
            const auto mods = static_cast<unsigned char>(frame.payload[1]);
            const auto encoded = terminal_.encode_key(
                key, {(mods & 1U) != 0, (mods & 2U) != 0, (mods & 4U) != 0, (mods & 8U) != 0});
            bytes = QByteArray(encoded.data(), static_cast<qsizetype>(encoded.size()));
            break;
        }
        case wire::Kind::resize: {
            if (frame.payload.size() != 4)
                throw std::runtime_error("Invalid resize message");
            QDataStream in(frame.payload);
            quint16 columns{}, rows{};
            in >> columns >> rows;
            TerminalSize size{columns, rows};
            terminal_.resize(size);
            if (!pty_.resize(size))
                throw std::runtime_error("PTY resize failed");
            dirty_ = true;
            schedule();
            return;
        }
        default:
            throw std::runtime_error("Unexpected client message");
        }
        if (!pty_.writeBytes(bytes))
            throw std::runtime_error("PTY input queue full");
    }
    QLockFile lock_;
    Terminal terminal_;
    posix::PtyProcess pty_;
    QLocalServer server_;
    QPointer<QLocalSocket> client_;
    QByteArray buffer_;
    QTimer timer_;
    bool dirty_{true};
};
} // namespace
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (app.arguments().size() != 3) {
        qCritical() << "Usage: lapis_session_service SOCKET DIRECTORY";
        return 2;
    }
    try {
        SessionService service(app.arguments().at(1), QDir(app.arguments().at(2)));
        return app.exec();
    } catch (const std::exception& error) {
        qCritical().noquote() << error.what();
        return 1;
    }
}
