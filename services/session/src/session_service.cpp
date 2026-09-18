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
#include <limits>
#include <memory>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace {
using namespace lapis::session;
int hex_nibble(char16_t value) {
    if (value >= u'0' && value <= u'9')
        return value - u'0';
    if (value >= u'a' && value <= u'f')
        return value - u'a' + 10;
    if (value >= u'A' && value <= u'F')
        return value - u'A' + 10;
    return -1;
}
QByteArray parse_session_id(const QString& value) {
    QByteArray id;
    if (value.size() != 32)
        throw std::invalid_argument("Session ID must be exactly 32 hexadecimal characters");
    for (qsizetype index = 0; index < value.size(); index += 2) {
        const int high = hex_nibble(value[index].unicode());
        const int low = hex_nibble(value[index + 1].unicode());
        if (high < 0 || low < 0)
            throw std::invalid_argument("Session ID must be exactly 32 hexadecimal characters");
        id.append(static_cast<char>((static_cast<unsigned int>(high) << 4U) |
                                    static_cast<unsigned int>(low)));
    }
    if (id == QByteArray(16, '\0'))
        throw std::invalid_argument("Session ID cannot be zero");
    return id;
}
class SessionService final : public QObject {
  public:
    SessionService(const QString& endpoint, const QByteArray& requested_session_id,
                   const LaunchSpec& launch)
        : lock_(endpoint + QStringLiteral(".lock")), terminal_(launch.size, limits()),
          fingerprint_(launch_fingerprint(launch)),
          identity_{requested_session_id, wire::new_id()} {
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
        ack_timer_.setSingleShot(true);
        ack_timer_.setInterval(3000);
        connect(&ack_timer_, &QTimer::timeout, this, [this] {
            if (client_ && !ready_) {
                send_status(client_, wire::StatusCode::rejected,
                            "Attachment ready acknowledgement timed out");
                detach_client();
            }
        });
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
        for (auto* retired : retired_)
            disconnect(retired, nullptr, this, nullptr);
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
            send_status(client_, wire::StatusCode::ended, message);
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
            if (!stopping_)
                send_status(incoming, wire::StatusCode::overloaded, "Too many pending attachments");
            incoming->disconnectFromServer();
            return;
        }
        pending_.insert(incoming);
        incoming->setReadBufferSize(75); // v3 attach is exactly 74 framed bytes.
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
        try {
            bytes += incoming->readAll();
            if (bytes.size() < 5)
                return;
            QDataStream header(bytes);
            quint32 frame_size{};
            quint8 kind{};
            header >> frame_size >> kind;
            if (frame_size != 70 || kind != static_cast<quint8>(wire::Kind::attach) ||
                bytes.size() > 74)
                throw std::runtime_error("Launch mismatch or incompatible attachment protocol");
            wire::Frame frame;
            if (!wire::take_frame(bytes, frame))
                return;
            const auto request = wire::decode_attach(frame.payload);
            if (request.fingerprint != fingerprint_)
                throw std::runtime_error("Launch mismatch or incompatible attachment protocol");
            if ((request.mode == wire::AttachMode::reconnect && request.expected != identity_) ||
                (request.mode == wire::AttachMode::create &&
                 request.expected.session_id != identity_.session_id)) {
                send_status(incoming, wire::StatusCode::replaced,
                            "Session identity mismatch: the endpoint belongs to another session");
                incoming->disconnectFromServer();
                return;
            }
            pending_.remove(incoming);
            disconnect(incoming, nullptr, this, nullptr);
            activate(incoming);
        } catch (const std::exception& error) {
            reject_attachment(incoming, QString::fromUtf8(error.what()));
        }
    }
    void activate(QLocalSocket* incoming) {
        if (generation_ == std::numeric_limits<quint64>::max()) {
            send_status(incoming, wire::StatusCode::overloaded, "Attachment generation overflow");
            incoming->disconnectFromServer();
            return;
        }
        ++generation_;
        attachment_ = {.identity = identity_, .generation = generation_};
        ready_ = false;
        snapshot_in_flight_ = false;
        ack_timer_.start(); // Bound synchronization even if hello cannot drain.
        if (client_) {
            send_status(client_, wire::StatusCode::replaced, "Session client replaced");
            disconnect(client_, nullptr, this, nullptr);
            retire(client_);
        }
        client_ = incoming;
        buffer_.clear();
        incoming->setReadBufferSize(wire::max_frame_bytes + 4);
        connect(incoming, &QLocalSocket::readyRead, this, [this, incoming] {
            if (client_ == incoming)
                receive();
        });
        connect(incoming, &QLocalSocket::bytesWritten, this, [this] { schedule(); });
        connect(incoming, &QLocalSocket::disconnected, this, [this, incoming] {
            if (client_ == incoming)
                detach_client();
        });
        hello();
    }
    void hello() {
        if (!client_ || !process_started_ || stopping_)
            return;
        client_->write(wire::frame(
            wire::Kind::hello,
            wire::encode_hello({.attachment = attachment_, .pid = quint64(pty_.processId())})));
        dirty_ = true;
        schedule();
    }
    void schedule() {
        if (client_ && process_started_ && (!snapshot_in_flight_ || ready_) && dirty_ &&
            !timer_.isActive() && !stopping_)
            timer_.start();
    }
    void publish() {
        if (!client_ || client_->state() != QLocalSocket::ConnectedState || !dirty_)
            return;
        if (!ready_ && snapshot_in_flight_)
            return;
        if (client_->bytesToWrite() != 0)
            return; // one replaceable snapshot in flight
        try {
            if (snapshot_sequence_ == std::numeric_limits<quint64>::max())
                throw std::overflow_error("Snapshot sequence overflow");
            const quint64 sequence = ++snapshot_sequence_;
            const auto bytes =
                wire::frame(wire::Kind::snapshot,
                            wire::encode_snapshot_message({.attachment = attachment_,
                                                           .sequence = sequence,
                                                           .snapshot = terminal_.snapshot()}));
            if (client_->write(bytes) < 0)
                throw std::runtime_error("Session socket write failed");
            dirty_ = false;
            if (!ready_) {
                ready_sequence_ = sequence;
                snapshot_in_flight_ = true;
            }
        } catch (const std::length_error&) {
            dirty_ = false;
            send_status(client_, wire::StatusCode::overloaded,
                        "Snapshot limit exceeded; session is still running");
            detach_client();
        } catch (const std::overflow_error&) {
            dirty_ = false;
            send_status(client_, wire::StatusCode::overloaded, "Snapshot sequence overflow");
            detach_client();
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
            send_status(client_, wire::StatusCode::rejected, QString::fromUtf8(error.what()));
            detach_client();
        }
    }
    void acknowledge(const QByteArray& payload) {
        const auto ready = wire::decode_ready(payload);
        if (ready.attachment != attachment_ || ready.sequence != ready_sequence_)
            throw std::runtime_error("Stale or mismatched ready acknowledgement");
        if (!ready_ && !snapshot_in_flight_)
            throw std::runtime_error("Unexpected ready acknowledgement");
        ready_ = true;
        snapshot_in_flight_ = false;
        ack_timer_.stop();
        schedule();
    }
    void handle(const wire::Frame& frame) {
        if (!process_started_ || stopping_)
            throw std::runtime_error("Session is not ready for input");
        if (frame.kind == wire::Kind::ready) {
            acknowledge(frame.payload);
            return;
        }
        const auto control = wire::decode_control(frame.payload);
        if (control.attachment != attachment_ || !ready_)
            throw std::runtime_error("Stale attachment or session is not ready for input");
        if (control.payload.size() > qsizetype{64} * 1024)
            throw std::runtime_error("Input message too large");
        QByteArray bytes;
        switch (frame.kind) {
        case wire::Kind::text:
            bytes = control.payload;
            break;
        case wire::Kind::paste: {
            const auto encoded = terminal_.encode_paste(std::string_view(
                control.payload.constData(), static_cast<std::size_t>(control.payload.size())));
            bytes = QByteArray(encoded.data(), static_cast<qsizetype>(encoded.size()));
            break;
        }
        case wire::Kind::key: {
            if (control.payload.size() != 2)
                throw std::runtime_error("Invalid key message");
            const auto key_value = static_cast<unsigned char>(control.payload[0]);
            if (key_value > static_cast<unsigned char>(TerminalKey::escape))
                throw std::runtime_error("Unknown key code");
            const auto key = static_cast<TerminalKey>(key_value);
            const auto mods = static_cast<unsigned char>(control.payload[1]);
            const auto encoded = terminal_.encode_key(
                key, {(mods & 1U) != 0, (mods & 2U) != 0, (mods & 4U) != 0, (mods & 8U) != 0});
            bytes = QByteArray(encoded.data(), static_cast<qsizetype>(encoded.size()));
            break;
        }
        case wire::Kind::resize: {
            if (control.payload.size() != 4)
                throw std::runtime_error("Invalid resize message");
            QDataStream in(control.payload);
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
    void send_status(QLocalSocket* socket, wire::StatusCode code, const QString& message) const {
        socket->write(wire::frame(wire::Kind::status,
                                  wire::encode_status({.code = code, .message = message})));
    }
    void reject_attachment(QLocalSocket* incoming, const QString& message = {}) {
        send_status(incoming, wire::StatusCode::rejected,
                    message.isEmpty() ? QStringLiteral("Invalid attachment request") : message);
        incoming->disconnectFromServer();
    }
    void retire(QLocalSocket* socket) {
        // Let a typed final status drain, but bound both time and retired sockets.
        if (retired_.size() >= 8) {
            auto* oldest = *retired_.begin();
            retired_.remove(oldest);
            oldest->abort();
            oldest->deleteLater();
        }
        retired_.insert(socket);
        connect(socket, &QObject::destroyed, this, [this, socket] { retired_.remove(socket); });
        socket->disconnectFromServer();
        if (socket->state() == QLocalSocket::UnconnectedState)
            socket->deleteLater();
        else
            QTimer::singleShot(1000, socket, &QObject::deleteLater);
    }
    void detach_client() {
        if (!client_)
            return;
        disconnect(client_, nullptr, this, nullptr);
        retire(client_);
        client_ = nullptr;
        buffer_.clear();
        ready_ = false;
        snapshot_in_flight_ = false;
        ack_timer_.stop();
    }
    QLockFile lock_;
    Terminal terminal_;
    posix::PtyProcess pty_;
    QLocalServer server_;
    QPointer<QLocalSocket> client_;
    QSet<QLocalSocket*> pending_;
    QSet<QLocalSocket*> retired_;
    QByteArray fingerprint_;
    wire::SessionIdentity identity_;
    wire::Attachment attachment_;
    QByteArray buffer_;
    QTimer timer_;
    QTimer ack_timer_;
    quint64 generation_{};
    quint64 snapshot_sequence_{};
    quint64 ready_sequence_{};
    bool dirty_{true};
    bool ready_{};
    bool snapshot_in_flight_{};
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
    bool has_session_id = arguments.size() > 1 && arguments.at(1) == QStringLiteral("--session-id");
    const int socket_index = has_session_id ? 3 : 1;
    const int program_index = has_session_id ? 5 : 3;
    if ((has_session_id && arguments.size() < 6) || (!has_session_id && arguments.size() < 4)) {
        qCritical() << "Usage: lapis_session_service [--session-id HEX32] SOCKET DIRECTORY PROGRAM "
                       "[ARG ...]";
        return 2;
    }
    try {
        const QByteArray session_id =
            has_session_id ? parse_session_id(arguments.at(2)) : wire::new_id();
        const auto launch = validate_launch({.program = arguments.at(program_index),
                                             .arguments = arguments.mid(program_index + 1),
                                             .directory = arguments.at(socket_index + 1)});
        SessionService service(posix::prepare_endpoint(arguments.at(socket_index)), session_id,
                               launch);
        return app.exec();
    } catch (const std::exception& error) {
        qCritical().noquote() << error.what();
        return 1;
    }
}
