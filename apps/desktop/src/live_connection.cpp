#include "live_connection.hpp"
#include <QDataStream>
#include <QDebug>
#include <QProcess>
#include <exception>
#include <utility>

namespace lapis::desktop {
namespace wire = session::wire;
SessionPreview::~SessionPreview() { live_.reset(); }
void SessionPreview::startLive(const QString& endpoint, const session::LaunchSpec& launch) {
    live_ = std::make_unique<LiveConnection>(*this, endpoint, launch);
}
void SessionPreview::applySnapshot(session::TerminalSnapshot snapshot) {
    snapshot_ = std::move(snapshot);
    live_snapshot_ready_ = live();
    emit snapshotChanged();
}
void SessionPreview::setActivity(const QString& activity) {
    activity_ = activity;
    emit snapshotChanged();
}
void SessionPreview::sendText(const QByteArray& bytes, bool paste) {
    if (live_)
        live_->send(paste ? wire::Kind::paste : wire::Kind::text, bytes);
}
void SessionPreview::sendKey(session::TerminalKey key, session::KeyModifiers modifiers) {
    const unsigned int mods = (modifiers.shift ? 1U : 0U) | (modifiers.control ? 2U : 0U) |
                              (modifiers.alt ? 4U : 0U) | (modifiers.super ? 8U : 0U);
    QByteArray bytes;
    bytes.append(static_cast<char>(key));
    bytes.append(static_cast<char>(mods));
    if (live_)
        live_->send(wire::Kind::key, bytes);
}
void SessionPreview::resizeTerminal(session::TerminalSize size) {
    if (live_)
        live_->resize(size);
}

LiveConnection::LiveConnection(SessionPreview& document, QString endpoint,
                               const session::LaunchSpec& launch)
    : document_(document), endpoint_(std::move(endpoint)),
      service_arguments_{QStringList{endpoint_, launch.directory, launch.program} +
                         launch.arguments},
      fingerprint_(session::launch_fingerprint(launch)), wanted_size_(launch.size) {
    socket_.setReadBufferSize(wire::max_frame_bytes + 4);
    retry_.setSingleShot(true);
    retry_.setInterval(100);
    handshake_.setSingleShot(true);
    handshake_.setInterval(3000);
    connect(&handshake_, &QTimer::timeout, this,
            [this] { fail(QStringLiteral("Session handshake timed out")); });
    connect(&retry_, &QTimer::timeout, this, [this] { connectSocket(); });
    connect(&socket_, &QLocalSocket::readyRead, this, [this] { receive(); });
    connect(&socket_, &QLocalSocket::connected, this, [this] {
        connected_ = true;
        retry_.stop();
        handshake_.start();
        QByteArray identity;
        QDataStream out(&identity, QIODevice::WriteOnly);
        out << wire::version << fingerprint_;
        socket_.write(wire::frame(wire::Kind::attach, identity));
    });
    connect(&socket_, &QLocalSocket::disconnected, this, [this] {
        if (failed_)
            return;
        fail(ready_ ? QStringLiteral("Disconnected")
                    : QStringLiteral("Disconnected before session handshake"));
    });
    connect(&socket_, &QLocalSocket::errorOccurred, this,
            [this](QLocalSocket::LocalSocketError error) {
                if (failed_)
                    return;
                if (connected_) {
                    fail(socket_.errorString());
                    return;
                }
                if (error != QLocalSocket::ServerNotFoundError &&
                    error != QLocalSocket::ConnectionRefusedError) {
                    fail(socket_.errorString());
                    return;
                }
                if (attempts_ >= max_attempts) {
                    fail(socket_.errorString());
                    return;
                }
                if (!launched_) {
                    launched_ = true;
                    QProcess process;
                    process.setProgram(QStringLiteral(LAPIS_SESSION_SERVICE_PATH));
                    process.setArguments(service_arguments_);
                    const QString log = endpoint_ + QStringLiteral(".log");
                    process.setStandardOutputFile(log, QIODevice::Append);
                    process.setStandardErrorFile(log, QIODevice::Append);
                    if (!process.startDetached()) {
                        fail(QStringLiteral("Could not start session service"));
                        return;
                    }
                }
                retry_.start();
            });
    QTimer::singleShot(0, this, [this] { connectSocket(); });
}
LiveConnection::~LiveConnection() {
    disconnect(&socket_, nullptr, this, nullptr);
    retry_.stop();
    handshake_.stop();
    socket_.abort();
}
void LiveConnection::connectSocket() {
    if (failed_ || connected_)
        return;
    ++attempts_;
    socket_.abort();
    buffer_.clear();
    socket_.connectToServer(endpoint_);
}
void LiveConnection::fail(const QString& message) {
    if (failed_)
        return;
    failed_ = true;
    ready_ = false;
    retry_.stop();
    handshake_.stop();
    report(message);
    socket_.abort();
}
void LiveConnection::report(const QString& message) {
    document_.setActivity(message);
    qInfo().noquote() << "Session:" << message;
}
void LiveConnection::send(wire::Kind kind, const QByteArray& payload) {
    if (failed_)
        return;
    if (!ready_) {
        report(QStringLiteral("Waiting for session connection"));
        return;
    }
    if (payload.size() > qsizetype{64} * 1024 ||
        socket_.bytesToWrite() + payload.size() > qint64{1024} * 1024) {
        report(QStringLiteral("Input queue full; input was not sent"));
        return;
    }
    if (socket_.write(wire::frame(kind, payload)) < 0)
        report(socket_.errorString());
}
void LiveConnection::resize(session::TerminalSize size) {
    if (size == wanted_size_)
        return;
    wanted_size_ = size;
    if (!ready_)
        return;
    QByteArray bytes;
    QDataStream out(&bytes, QIODevice::WriteOnly);
    out << quint16(size.columns) << quint16(size.rows);
    send(wire::Kind::resize, bytes);
}
void LiveConnection::receive() {
    try {
        buffer_ += socket_.readAll();
        if (buffer_.size() > wire::max_frame_bytes + 4)
            throw std::runtime_error("Session receive overflow");
        wire::Frame frame;
        while (wire::take_frame(buffer_, frame)) {
            if (frame.kind == wire::Kind::hello) {
                QDataStream in(frame.payload);
                quint32 version{};
                quint64 pid{};
                in >> version >> pid;
                if (ready_ || pid == 0 || version != wire::version ||
                    in.status() != QDataStream::Ok || !in.atEnd())
                    throw std::runtime_error("Incompatible session service");
                ready_ = true;
                handshake_.stop();
                report(QStringLiteral("Live terminal"));
                qInfo() << "Connected terminal PID" << pid;
                const auto wanted = wanted_size_;
                wanted_size_ = {1, 1};
                resize(wanted);
            } else if (frame.kind == wire::Kind::snapshot) {
                if (!ready_)
                    throw std::runtime_error("Snapshot before session handshake");
                document_.applySnapshot(wire::decode_snapshot(frame.payload));
            } else if (frame.kind == wire::Kind::status) {
                if (!ready_)
                    throw std::runtime_error(frame.payload.toStdString());
                fail(QString::fromUtf8(frame.payload));
                return;
            } else
                throw std::runtime_error("Unexpected service message");
        }
    } catch (const std::exception& error) {
        fail(QString::fromUtf8(error.what()));
    }
}
} // namespace lapis::desktop
