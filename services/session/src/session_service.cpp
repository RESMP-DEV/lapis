#include "platform/posix/local_endpoint.hpp"
#include "platform/posix/pty_process.hpp"
#include "transport/local_protocol.hpp"

#include <QCoreApplication>
#include <QDataStream>
#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QPointer>
#include <QSet>
#include <QTimer>

#include <exception>
#include <memory>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace {
using namespace lapis::session;
class SessionService final : public QObject {
  public:
    SessionService(const QString& endpoint, const LaunchSpec& launch)
        : lock_(endpoint + QStringLiteral(".lock")), terminal_(launch.size, limits()) {
        QByteArray identity;
        QDataStream identity_stream(&identity, QIODevice::WriteOnly);
        identity_stream << wire::version << launch_fingerprint(launch);
        expected_attachment_ = wire::frame(wire::Kind::attach, identity);
        terminal_.feed("\x1b]10;rgb:d9/de/e8\x1b\\\x1b]11;rgb:0d/13/1d\x1b\\");
        lock_.setStaleLockTime(0);
        if (!lock_.tryLock(0))
            throw std::runtime_error("Session service already owns endpoint");
        // Only the endpoint owner may trim its append log; racing GUI launches
        // must not truncate the log of an already-running service.
        QFile log(endpoint + QStringLiteral(".log"));
        if (log.size() > qint64{1024} * 1024 && log.open(QIODevice::ReadWrite))
            static_cast<void>(log.resize(0));
        if (QFileInfo::exists(endpoint)) {
            QLocalSocket existing;
            existing.connectToServer(endpoint);
            if (existing.waitForConnected(100) ||
                (existing.error() != QLocalSocket::ConnectionRefusedError &&
                 existing.error() != QLocalSocket::ServerNotFoundError))
                throw std::runtime_error("Socket endpoint is already in use or inaccessible");
        }
        QLocalServer::removeServer(endpoint);
        server_.setSocketOptions(QLocalServer::UserAccessOption);
        if (!server_.listen(endpoint))
            throw std::runtime_error(server_.errorString().toStdString());
        struct stat bound_socket{};
        if (::lstat(QFile::encodeName(endpoint).constData(), &bound_socket) != 0 ||
            !S_ISSOCK(bound_socket.st_mode) || bound_socket.st_uid != ::getuid() ||
            (static_cast<unsigned int>(bound_socket.st_mode) & 0077U) != 0U)
            throw std::runtime_error("Bound socket is not private to the current user");
        timer_.setSingleShot(true);
        timer_.setInterval(16);
        connect(&timer_, &QTimer::timeout, this, [this] { publish(); });
        connect(&server_, &QLocalServer::newConnection, this, [this] { attach(); });
        connect(&pty_, &posix::PtyProcess::started, this, [this] {
            process_started_ = true;
            hello();
        });
        connect(&pty_, &posix::PtyProcess::output, this, [this](const QByteArray& bytes) {
            try {
                terminal_.feed(
                    std::string_view(bytes.constData(), static_cast<std::size_t>(bytes.size())));
                const auto replies = terminal_.take_replies();
                if (!replies.empty() && pty_.processId() != 0 &&
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
                [this](int code, QProcess::ExitStatus status) {
                    if (status == QProcess::CrashExit)
                        stop(QStringLiteral("Process terminated by signal (%1)").arg(code),
                             128 + code);
                    else
                        stop(QStringLiteral("Process exited (%1)").arg(code), code);
                });
        pty_.start(launch);
    }

    ~SessionService() override {
        disconnect(&pty_, nullptr, this, nullptr);
        disconnect(&server_, nullptr, this, nullptr);
        disconnect(&timer_, nullptr, this, nullptr);
        if (client_)
            disconnect(client_, nullptr, this, nullptr);
        for (auto* pending : pending_)
            disconnect(pending, nullptr, this, nullptr);
    }

  private:
    static TerminalLimits limits() {
        TerminalLimits result;
        result.max_cells = wire::max_cells;
        result.max_grapheme_codepoints = wire::max_codepoints;
        result.max_input_bytes = std::size_t{64} * 1024U;
        return result;
    }
    void stop(const QString& message, int exit_code = 1) {
        if (stopping_)
            return;
        stopping_ = true;
        qInfo().noquote() << message;
        if (client_) {
            client_->write(wire::frame(wire::Kind::status, message.toUtf8()));
            client_->flush();
        }
        QTimer::singleShot(50, QCoreApplication::instance(),
                           [exit_code] { QCoreApplication::exit(exit_code); });
    }
    void attach() {
        auto* incoming = server_.nextPendingConnection();
        if (!incoming)
            return;
        connect(incoming, &QLocalSocket::disconnected, incoming, &QObject::deleteLater);
        if (stopping_ || pending_.size() >= 8) {
            incoming->disconnectFromServer();
            return;
        }
        pending_.insert(incoming);
        incoming->setReadBufferSize(expected_attachment_.size() + 1);
        const auto bytes = std::make_shared<QByteArray>();
        connect(incoming, &QLocalSocket::readyRead, this,
                [this, incoming, bytes] { authenticate(incoming, *bytes); });
        connect(incoming, &QLocalSocket::disconnected, this,
                [this, incoming] { pending_.remove(incoming); });
        const QPointer<QLocalSocket> guarded(incoming);
        QTimer::singleShot(3000, this, [this, guarded] {
            if (guarded && pending_.contains(guarded))
                guarded->disconnectFromServer();
        });
        if (incoming->bytesAvailable())
            authenticate(incoming, *bytes);
    }
    void authenticate(QLocalSocket* incoming, QByteArray& bytes) {
        bytes += incoming->readAll();
        if (bytes.size() < expected_attachment_.size() && expected_attachment_.startsWith(bytes))
            return;
        if (bytes != expected_attachment_) {
            incoming->write(wire::frame(wire::Kind::status,
                                        "Launch mismatch or incompatible attachment protocol"));
            incoming->disconnectFromServer();
            return;
        }
        pending_.remove(incoming);
        disconnect(incoming, nullptr, this, nullptr);
        activate(incoming);
    }
    void activate(QLocalSocket* incoming) {
        if (client_) {
            disconnect(client_, nullptr, this, nullptr);
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
        hello();
    }
    void hello() {
        if (!client_ || !process_started_ || stopping_)
            return;
        QByteArray hello;
        QDataStream out(&hello, QIODevice::WriteOnly);
        out << wire::version << quint64(pty_.processId());
        client_->write(wire::frame(wire::Kind::hello, hello));
        dirty_ = true;
        schedule();
    }
    void schedule() {
        if (client_ && process_started_ && dirty_ && !timer_.isActive() && !stopping_)
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
        } catch (const std::length_error&) {
            dirty_ = false;
            client_->write(wire::frame(wire::Kind::status,
                                       "Snapshot limit exceeded; session is still "
                                       "running. Reattach after reducing output."));
            client_->disconnectFromServer();
        } catch (const std::exception& error) {
            stop(QString::fromUtf8(error.what()));
        }
    }
    void receive() {
        if (!client_ || client_->state() != QLocalSocket::ConnectedState)
            return;
        try {
            buffer_ += client_->read(wire::max_frame_bytes + 4 - buffer_.size());
            wire::Frame frame;
            qsizetype consumed{};
            for (int processed = 0; processed < 64; ++processed) {
                if (!wire::take_frame(buffer_, consumed, frame)) {
                    if (consumed != 0)
                        buffer_.remove(0, consumed);
                    return;
                }
                handle(frame);
            }
            if (buffer_.size() > wire::max_frame_bytes + 4)
                throw std::runtime_error("Session input overflow");
            if (consumed != 0)
                buffer_.remove(0, consumed);
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
        if (!process_started_ || stopping_)
            throw std::runtime_error("Session is not ready for input");
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
            const auto key_value = static_cast<unsigned char>(frame.payload[0]);
            if (key_value > static_cast<unsigned char>(TerminalKey::escape))
                throw std::runtime_error("Unknown key code");
            const auto key = static_cast<TerminalKey>(key_value);
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
            if (columns == 0 || rows == 0 || quint32(columns) * rows > wire::max_cells)
                throw std::runtime_error("Invalid terminal geometry");
            if (!pty_.resize(size))
                throw std::runtime_error("PTY resize failed");
            try {
                terminal_.resize(size);
            } catch (const std::exception& error) {
                stop(QString::fromUtf8(error.what()));
                return; // A failed engine resize cannot be presented as synchronized.
            }
            const auto replies = terminal_.take_replies();
            if (!replies.empty() && !pty_.writeBytes(QByteArray(
                                        replies.data(), static_cast<qsizetype>(replies.size()))))
                throw std::runtime_error("PTY reply queue overflow");
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
    QSet<QLocalSocket*> pending_;
    QByteArray expected_attachment_;
    QByteArray buffer_;
    QTimer timer_;
    bool dirty_{true};
    bool process_started_{};
    bool stopping_{};
};
} // namespace
int main(int argc, char** argv) {
    QStringList arguments;
    for (int index = 0; index < argc; ++index)
        arguments.append(QString::fromLocal8Bit(argv[index]));
    int application_argc = 1;
    QCoreApplication app(application_argc, argv);
    if (arguments.size() < 4) {
        qCritical() << "Usage: lapis_session_service SOCKET DIRECTORY PROGRAM [ARG ...]";
        return 2;
    }
    try {
        const auto launch = validate_launch({.program = arguments.at(3),
                                             .arguments = arguments.mid(4),
                                             .directory = arguments.at(2)});
        SessionService service(posix::prepare_endpoint(arguments.at(1)), launch);
        return app.exec();
    } catch (const std::exception& error) {
        qCritical().noquote() << error.what();
        return 1;
    }
}
