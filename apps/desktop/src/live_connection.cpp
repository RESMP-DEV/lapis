#include "live_connection.hpp"
#include "platform/posix/local_endpoint.hpp"
#include "session_descriptor.hpp"
#include <QDataStream>
#include <QDebug>
#include <QFile>
#include <QFutureWatcher>
#include <QProcess>
#include <QPromise>
#include <QThreadPool>
#include <exception>
#include <stdexcept>
#include <utility>

namespace lapis::desktop {
namespace wire = session::wire;
SessionPreview::~SessionPreview() { live_.reset(); }
void SessionPreview::startLive(const QString& endpoint, const session::LaunchSpec& launch,
                               wire::AttachMode mode) {
    live_ = std::make_unique<LiveConnection>(*this, endpoint, launch, mode);
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

void SessionPreview::reconnect() {
    if (live_)
        live_->begin(wire::AttachMode::reconnect);
}
void SessionPreview::discoverSession() {
    if (live_)
        live_->begin(wire::AttachMode::discover);
}
void SessionPreview::startNewSession() {
    if (live_)
        live_->begin(wire::AttachMode::create);
}
void SessionPreview::setConnection(const QString& state, bool input_ready) {
    connection_state_ = state;
    input_ready_ = input_ready;
    if (!input_ready)
        live_snapshot_ready_ = false;
    emit connectionChanged();
}
void SessionPreview::setServiceIdentity(const QByteArray& identity) {
    service_session_id_ = QString::fromLatin1(identity.toHex());
    emit connectionChanged();
}
LiveConnection::LiveConnection(SessionPreview& document, QString endpoint,
                               const session::LaunchSpec& launch, wire::AttachMode mode)
    : document_(document), endpoint_(std::move(endpoint)),
      service_arguments_{QStringList{endpoint_, launch.directory, launch.program} +
                         launch.arguments},
      fingerprint_(session::launch_fingerprint(launch)), wanted_size_(launch.size) {
    retry_.setSingleShot(true);
    retry_.setInterval(100);
    handshake_.setSingleShot(true);
    handshake_.setInterval(5000);
    connect(&handshake_, &QTimer::timeout, this,
            [this] { fail(QStringLiteral("Session synchronization timed out")); });
    connect(&retry_, &QTimer::timeout, this, [this] { connectSocket(); });
    QTimer::singleShot(0, this, [this, mode] { begin(mode); });
}
LiveConnection::~LiveConnection() {
    retry_.stop();
    handshake_.stop();
    if (socket_) {
        disconnect(socket_.get(), nullptr, this, nullptr);
        socket_->abort();
    }
}
void LiveConnection::begin(wire::AttachMode mode) {
    if (!failed_)
        return;
    failed_ = false;
    connected_ = false;
    ready_ = false;
    attempts_ = 0;
    last_sequence_ = 0;
    attachment_.reset();
    buffer_.clear();
    retry_.stop();
    handshake_.stop();
    document_.setConnection(QStringLiteral("connecting"), false);
    report(QStringLiteral("Connecting"));
    try {
        endpoint_ = session::posix::prepare_endpoint(endpoint_);
        request_ = {.mode = mode, .fingerprint = fingerprint_, .expected = {}};
        if (mode == wire::AttachMode::reconnect) {
            const auto saved = session::read_descriptor(endpoint_, fingerprint_);
            if (!saved) {
                fail(QStringLiteral(
                    "No saved session. Discover an existing session or start a new one."));
                return;
            }
            request_.expected = *saved;
        } else if (mode == wire::AttachMode::create) {
            request_.expected.session_id = wire::new_id();
            QProcess service;
            service.setProgram(QStringLiteral(LAPIS_SESSION_SERVICE_PATH));
            service.setArguments(
                QStringList{QStringLiteral("--session-id"),
                            QString::fromLatin1(request_.expected.session_id.toHex())} +
                service_arguments_);
            const QString log = endpoint_ + QStringLiteral(".log");
            service.setStandardOutputFile(log, QIODevice::Append);
            service.setStandardErrorFile(log, QIODevice::Append);
            if (!service.startDetached())
                throw std::runtime_error("Could not start session service");
        }
        connectSocket();
    } catch (const std::exception& error) {
        fail(QString::fromUtf8(error.what()));
    }
}
void LiveConnection::resetSocket() {
    if (socket_) {
        disconnect(socket_.get(), nullptr, this, nullptr);
        socket_->abort();
    }
    socket_ = std::make_unique<QLocalSocket>();
    socket_->setReadBufferSize(wire::max_frame_bytes + 4);
    connect(socket_.get(), &QLocalSocket::readyRead, this, [this] { receive(); });
    connect(socket_.get(), &QLocalSocket::connected, this, [this] {
        connected_ = true;
        retry_.stop();
        handshake_.start();
        socket_->write(wire::frame(wire::Kind::attach, wire::encode_attach(request_)));
    });
    connect(socket_.get(), &QLocalSocket::disconnected, this, [this] {
        fail(QStringLiteral("Disconnected. Reconnect to verify the saved session."));
    });
    connect(socket_.get(), &QLocalSocket::errorOccurred, this,
            [this](QLocalSocket::LocalSocketError error) {
                if (failed_)
                    return;
                const bool absent = error == QLocalSocket::ServerNotFoundError ||
                                    error == QLocalSocket::ConnectionRefusedError;
                if (!connected_ && absent && attempts_ < max_attempts) {
                    retry_.start();
                    return;
                }
                fail(socket_->errorString());
            });
}
void LiveConnection::connectSocket() {
    if (failed_ || connected_)
        return;
    ++attempts_;
    resetSocket();
    buffer_.clear();
    socket_->connectToServer(endpoint_);
}
void LiveConnection::fail(const QString& message, wire::StatusCode code) {
    if (failed_)
        return;
    failed_ = true;
    ready_ = false;
    connected_ = false;
    retry_.stop();
    handshake_.stop();
    const auto state = code == wire::StatusCode::ended      ? QStringLiteral("ended")
                       : code == wire::StatusCode::replaced ? QStringLiteral("replaced")
                                                            : QStringLiteral("disconnected");
    document_.setConnection(state, false);
    report(message);
    if (socket_)
        socket_->abort();
}
void LiveConnection::report(const QString& message) {
    document_.setActivity(message);
    qInfo().noquote() << "Session:" << message;
}
void LiveConnection::send(wire::Kind kind, const QByteArray& payload) {
    if (!ready_ || failed_ || !attachment_) {
        report(QStringLiteral("Input was not sent: session is not synchronized."));
        return;
    }
    if (payload.size() > qsizetype{64} * 1024 ||
        socket_->bytesToWrite() + payload.size() + 45 > qint64{1024} * 1024) {
        report(QStringLiteral("Input queue full; input was not sent"));
        return;
    }
    try {
        const auto bytes = wire::frame(kind, wire::encode_control({*attachment_, payload}));
        if (socket_->write(bytes) != bytes.size())
            fail(QStringLiteral("Session input could not be queued"));
    } catch (const std::exception& error) {
        fail(QString::fromUtf8(error.what()));
    }
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
void LiveConnection::acceptHello(const wire::Hello& hello) {
    if (attachment_)
        throw std::runtime_error("Duplicate session hello");
    if ((request_.mode == wire::AttachMode::reconnect &&
         hello.attachment.identity != request_.expected) ||
        (request_.mode == wire::AttachMode::create &&
         hello.attachment.identity.session_id != request_.expected.session_id)) {
        fail(QStringLiteral("The saved session has ended or been replaced."),
             wire::StatusCode::replaced);
        return;
    }
    attachment_ = hello.attachment;
    document_.setServiceIdentity(attachment_->identity.session_id);
    document_.setConnection(QStringLiteral("synchronizing"), false);
    report(QStringLiteral("Restoring terminal screen"));
    qInfo() << "Connected terminal PID" << hello.pid;
}
void LiveConnection::acceptSnapshot(wire::SnapshotMessage message) {
    if (!attachment_ || message.attachment != *attachment_ || message.sequence <= last_sequence_)
        throw std::runtime_error("Stale or mismatched terminal snapshot");
    const bool initial = last_sequence_ == 0;
    last_sequence_ = message.sequence;
    document_.applySnapshot(std::move(message.snapshot));
    if (initial)
        persistIdentity();
}
void LiveConnection::persistIdentity() {
    if (!attachment_)
        throw std::runtime_error("Cannot persist an unbound session");
    const auto expected = *attachment_;
    const auto sequence = last_sequence_;
    descriptor_write_ = std::make_unique<QFutureWatcher<QString>>();
    auto* watcher = descriptor_write_.get();
    connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher, expected, sequence] {
        if (failed_ || attachment_ != expected || last_sequence_ != sequence ||
            descriptor_write_.get() != watcher)
            return;
        const auto error = watcher->result();
        if (!error.isEmpty()) {
            fail(error);
            return;
        }
        try {
            finishSynchronization();
        } catch (const std::exception& failure) {
            fail(QString::fromUtf8(failure.what()));
        }
    });
    QPromise<QString> promise;
    watcher->setFuture(promise.future());
    QThreadPool::globalInstance()->start([promise = std::move(promise), endpoint = endpoint_,
                                          fingerprint = fingerprint_,
                                          identity = expected.identity]() mutable {
        promise.start();
        QString error;
        try {
            session::write_descriptor(endpoint, fingerprint, identity);
        } catch (const std::exception& failure) {
            error = QString::fromUtf8(failure.what());
        }
        promise.addResult(error);
        promise.finish();
    });
}
void LiveConnection::finishSynchronization() {
    if (!attachment_)
        throw std::runtime_error("Cannot acknowledge an unbound session");
    const auto ack =
        wire::frame(wire::Kind::ready, wire::encode_ready({*attachment_, last_sequence_}));
    if (socket_->write(ack) != ack.size())
        throw std::runtime_error("Could not acknowledge restored screen");
    ready_ = true;
    handshake_.stop();
    document_.setConnection(QStringLiteral("ready"), true);
    report(QStringLiteral("Live terminal"));
    const auto wanted = wanted_size_;
    wanted_size_ = {1, 1};
    resize(wanted);
}
void LiveConnection::handle(const wire::Frame& frame) {
    switch (frame.kind) {
    case wire::Kind::hello:
        acceptHello(wire::decode_hello(frame.payload));
        return;
    case wire::Kind::snapshot:
        acceptSnapshot(wire::decode_snapshot_message(frame.payload));
        return;
    case wire::Kind::status: {
        const auto status = wire::decode_status(frame.payload);
        fail(status.message, status.code);
        return;
    }
    default:
        throw std::runtime_error("Unexpected service message");
    }
}
void LiveConnection::receive() {
    try {
        buffer_ += socket_->readAll();
        wire::Frame frame;
        qsizetype consumed{};
        while (!failed_ && wire::take_frame(buffer_, consumed, frame))
            handle(frame);
        if (consumed != 0)
            buffer_.remove(0, consumed);
        if (buffer_.size() > wire::max_frame_bytes + 4)
            throw std::runtime_error("Session receive overflow");
    } catch (const std::exception& error) {
        fail(QString::fromUtf8(error.what()));
    }
}
} // namespace lapis::desktop
