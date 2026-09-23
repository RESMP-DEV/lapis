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
#include <limits>
#include <stdexcept>
#include <utility>

namespace lapis::desktop {
namespace wire = session::wire;
namespace {
constexpr int codex_sync_timeout_ms = 15000;
constexpr int terminal_sync_timeout_ms = 5000;
constexpr int history_timeout_ms = 5000;
} // namespace
SessionPreview::~SessionPreview() { live_.reset(); }
void SessionPreview::startLive(const QString& endpoint, const session::LaunchSpec& launch,
                               wire::AttachMode mode) {
    live_ = std::make_unique<LiveConnection>(*this, endpoint, launch, mode);
}
void SessionPreview::restoreLive(const WorkspaceEntry& entry) {
    setSessionId(QString::fromLatin1(entry.identity.session_id.toHex()));
    live_ = std::make_unique<LiveConnection>(*this, entry);
}
std::optional<WorkspaceEntry> SessionPreview::reconnectEntry() const {
    return live_ ? live_->reconnectEntry() : std::nullopt;
}
void SessionPreview::applySnapshot(session::TerminalSnapshot snapshot) {
    live_snapshot_ = std::move(snapshot);
    live_snapshot_received_ = true;
    live_snapshot_ready_ = live();
    if (!history_active_) {
        snapshot_ = live_snapshot_;
        emit snapshotChanged();
    }
}
void SessionPreview::beginHistoryRequest() {
    history_active_ = true;
    history_request_pending_ = true;
    emit historyChanged();
    emit connectionChanged();
}
void SessionPreview::completeHistoryRequest(quint64 page_id, session::TerminalSnapshot snapshot,
                                            const QString& message) {
    history_active_ = true;
    history_request_pending_ = false;
    history_page_id_ = page_id;
    history_message_ = message;
    snapshot_ = std::move(snapshot);
    emit historyChanged();
    emit connectionChanged();
    emit snapshotChanged();
}
void SessionPreview::failHistoryRequest(const QString& message) {
    if (history_request_pending_) {
        history_active_ = true;
        history_request_pending_ = false;
        history_message_ = message;
        emit historyChanged();
        emit connectionChanged();
    }
}
void SessionPreview::cancelHistoryRequests() {
    history_request_pending_ = false;
    emit historyChanged();
    emit connectionChanged();
}
void SessionPreview::setHistoryRequestId(quint64 request_id) {
    if (request_id != 0)
        beginHistoryRequest();
}
void SessionPreview::olderHistory() {
    if (!live_ || history_request_pending_)
        return;
    live_->requestHistory(wire::HistoryDirection::older, history_active_ ? history_page_id_ : 0);
}
void SessionPreview::newerHistory() {
    if (!live_ || history_request_pending_ || history_page_id_ == 0)
        return;
    live_->requestHistory(wire::HistoryDirection::newer, history_page_id_);
}
void SessionPreview::returnToLive() {
    if (live_)
        live_->cancelHistoryRequest();
    history_active_ = false;
    history_request_pending_ = false;
    history_page_id_ = 0;
    history_message_.clear();
    if (live_snapshot_received_) {
        snapshot_ = live_snapshot_;
        if (live_)
            live_->applyWantedSize();
    }
    emit historyChanged();
    emit connectionChanged();
    emit snapshotChanged();
}
void SessionPreview::setActivity(const QString& activity) {
    activity_ = activity;
    emit snapshotChanged();
}
void SessionPreview::sendText(const QByteArray& bytes, bool paste) {
    if (history_active_ || history_request_pending_)
        return;
    if (live_)
        live_->send(paste ? wire::Kind::paste : wire::Kind::text, bytes);
}
void SessionPreview::sendKey(session::TerminalKey key, session::KeyModifiers modifiers) {
    if (history_active_ || history_request_pending_)
        return;
    const unsigned int mods = (modifiers.shift ? 1U : 0U) | (modifiers.control ? 2U : 0U) |
                              (modifiers.alt ? 4U : 0U) | (modifiers.super ? 8U : 0U);
    QByteArray bytes;
    bytes.append(static_cast<char>(key));
    bytes.append(static_cast<char>(mods));
    if (live_)
        live_->send(wire::Kind::key, bytes);
}
void SessionPreview::resizeTerminal(session::TerminalSize size) {
    if (!live_)
        return;
    if (history_active_ || history_request_pending_) {
        live_->setWantedSize(size);
        return;
    }
    live_->resize(size);
}

void SessionPreview::reconnect() {
    if (live_)
        live_->begin(wire::AttachMode::reconnect);
}
void SessionPreview::discoverSession() {
    if (!live_)
        return;
    if (live_->reconnectOnly()) {
        setActivity(QStringLiteral("Cannot discover a reconnect-only workspace entry."));
        return;
    }
    live_->begin(wire::AttachMode::discover);
}
void SessionPreview::startNewSession() {
    if (!live_)
        return;
    if (live_->reconnectOnly()) {
        setActivity(
            QStringLiteral("Cannot create a session from a reconnect-only workspace entry."));
        return;
    }
    live_->begin(wire::AttachMode::create);
}
void SessionPreview::setConnection(const QString& state, bool input_ready) {
    connection_state_ = state;
    input_ready_ = input_ready;
    if (!input_ready) {
        live_snapshot_ready_ = false;
        invalidateAttention();
    }
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
      fingerprint_(session::launch_fingerprint(launch)), agent_(launch.agent),
      wanted_size_(launch.size) {
    if (launch.agent == session::AgentMode::codex)
        service_arguments_.prepend(QStringLiteral("--codex"));
    else if (launch.agent == session::AgentMode::claude)
        service_arguments_.prepend(QStringLiteral("--claude"));
    initialize(launch.agent == session::AgentMode::codex ? codex_sync_timeout_ms
                                                         : terminal_sync_timeout_ms,
               mode);
}
LiveConnection::LiveConnection(SessionPreview& document, const WorkspaceEntry& entry)
    : document_(document), endpoint_(entry.endpoint), fingerprint_(entry.fingerprint),
      expected_identity_(entry.identity), verified_entry_(entry), agent_(entry.agent),
      reconnect_only_{true} {
    initialize(entry.agent == session::AgentMode::codex ? codex_sync_timeout_ms
                                                        : terminal_sync_timeout_ms,
               wire::AttachMode::reconnect);
}
void LiveConnection::initialize(int synchronization_timeout, wire::AttachMode mode) {
    retry_.setSingleShot(true);
    retry_.setInterval(100);
    handshake_.setSingleShot(true);
    handshake_.setInterval(synchronization_timeout);
    connect(&handshake_, &QTimer::timeout, this,
            [this] { fail(QStringLiteral("Session synchronization timed out")); });
    connect(&retry_, &QTimer::timeout, this, [this] { connectSocket(); });
    history_timeout_.setSingleShot(true);
    history_timeout_.setInterval(history_timeout_ms);
    connect(&history_timeout_, &QTimer::timeout, this, [this] {
        if (!outstanding_history_request_)
            return;
        const auto request_id = *outstanding_history_request_;
        outstanding_history_request_.reset();
        document_.setHistoryRequestId(0);
        document_.failHistoryRequest(QStringLiteral("History request timed out."));
        rememberCanceledHistoryRequest(request_id);
    });
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
    if (reconnect_only_ && mode != wire::AttachMode::reconnect) {
        report(
            QStringLiteral("A reconnect-only workspace entry cannot create or discover sessions."));
        return;
    }
    failed_ = false;
    connected_ = false;
    ready_ = false;
    attempts_ = 0;
    last_sequence_ = 0;
    attachment_.reset();
    buffer_.clear();
    invalidateHistory();
    retry_.stop();
    handshake_.stop();
    document_.setConnection(QStringLiteral("connecting"), false);
    report(QStringLiteral("Connecting"));
    try {
        endpoint_ = session::posix::prepare_endpoint(endpoint_);
        request_ = {.mode = mode, .fingerprint = fingerprint_, .expected = {}};
        if (mode == wire::AttachMode::reconnect && expected_identity_) {
            request_.expected = *expected_identity_;
        } else if (mode == wire::AttachMode::reconnect) {
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
            // Publish only after spawn succeeds. A failed detached launch leaves
            // the card without an identity it never actually attached to.
            document_.setSessionId(QString::fromLatin1(request_.expected.session_id.toHex()));
        }
        connectSocket();
    } catch (const std::exception& error) {
        fail(QString::fromUtf8(error.what()));
    }
}

void LiveConnection::invalidateHistory() {
    if (outstanding_history_request_)
        rememberCanceledHistoryRequest(*outstanding_history_request_);
    history_timeout_.stop();
    outstanding_history_request_.reset();
    document_.cancelHistoryRequests();
}

void LiveConnection::rememberCanceledHistoryRequest(quint64 request_id) {
    canceled_history_requests_.insert(request_id);
    while (canceled_history_requests_.size() > 128)
        canceled_history_requests_.remove(*canceled_history_requests_.begin());
}

void LiveConnection::cancelHistoryRequest() {
    if (outstanding_history_request_) {
        rememberCanceledHistoryRequest(*outstanding_history_request_);
        outstanding_history_request_.reset();
    }
    history_timeout_.stop();
    document_.cancelHistoryRequests();
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
    invalidateHistory();
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
bool LiveConnection::send(wire::Kind kind, const QByteArray& payload) {
    if (!ready_ || failed_ || !attachment_) {
        report(QStringLiteral("Input was not sent: session is not synchronized."));
        return false;
    }
    if (payload.size() > qsizetype{64} * 1024 ||
        socket_->bytesToWrite() + payload.size() + 45 > qint64{1024} * 1024) {
        report(QStringLiteral("Input queue full; input was not sent"));
        return false;
    }
    try {
        const auto bytes = wire::frame(kind, wire::encode_control({*attachment_, payload}));
        if (socket_->write(bytes) != bytes.size()) {
            fail(QStringLiteral("Session input could not be queued"));
            return false;
        }
    } catch (const std::exception& error) {
        fail(QString::fromUtf8(error.what()));
        return false;
    }
    return true;
}
void LiveConnection::resize(session::TerminalSize size) {
    wanted_size_requested_ = true;
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

void LiveConnection::setWantedSize(session::TerminalSize size) {
    wanted_size_requested_ = true;
    wanted_size_ = size;
}

void LiveConnection::applyWantedSize() {
    const auto wanted = wanted_size_;
    wanted_size_ = {1, 1};
    resize(wanted);
}

void LiveConnection::requestHistory(wire::HistoryDirection direction, quint64 reference) {
    if (!ready_ || failed_ || !attachment_ || outstanding_history_request_)
        return;
    if (direction == wire::HistoryDirection::newer && reference == 0)
        throw std::runtime_error("Newer history requires a page");
    if (history_request_ids_exhausted_) {
        document_.failHistoryRequest(QStringLiteral("History browsing is unavailable."));
        return;
    }
    const auto request_id = next_history_request_id_;
    if (request_id == std::numeric_limits<quint64>::max())
        history_request_ids_exhausted_ = true;
    else
        ++next_history_request_id_;
    outstanding_history_request_ = request_id;
    document_.setHistoryRequestId(request_id);
    history_timeout_.start();
    if (!send(wire::Kind::history_request,
              wire::encode_history_request({request_id, reference, direction}))) {
        history_timeout_.stop();
        outstanding_history_request_.reset();
        document_.setHistoryRequestId(0);
        document_.failHistoryRequest(
            QStringLiteral("History request could not be queued; try again."));
    }
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
    if (request_.mode == wire::AttachMode::create || request_.mode == wire::AttachMode::discover)
        document_.setSessionId(QString::fromLatin1(attachment_->identity.session_id.toHex()));
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
    if (initial && !wanted_size_requested_)
        wanted_size_ = message.snapshot.size;
    document_.setSnapshotTiming(
        {{QStringLiteral("sequence"), QVariant::fromValue(message.sequence)},
         {QStringLiteral("pty_read_ns"), QVariant::fromValue(message.timing.pty_read_ns)},
         {QStringLiteral("parse_end_ns"), QVariant::fromValue(message.timing.parse_end_ns)},
         {QStringLiteral("publish_ns"), QVariant::fromValue(message.timing.publish_ns)}});
    document_.applySnapshot(std::move(message.snapshot));
    if (initial)
        persistIdentity();
}

void LiveConnection::acceptHistoryReply(wire::HistoryReply reply) {
    if (!attachment_ || reply.attachment != *attachment_) {
        if (outstanding_history_request_ == reply.request_id)
            cancelHistoryRequest();
        throw std::runtime_error("Mismatched history reply");
    }
    if (canceled_history_requests_.contains(reply.request_id))
        return;
    if (!outstanding_history_request_ || *outstanding_history_request_ != reply.request_id)
        throw std::runtime_error("Unexpected history reply");
    history_timeout_.stop();
    outstanding_history_request_.reset();
    document_.setHistoryRequestId(0);
    rememberCanceledHistoryRequest(reply.request_id);
    if (reply.page_id == 0) {
        document_.failHistoryRequest(reply.message.isEmpty()
                                         ? QStringLiteral("No history page is available.")
                                         : reply.message);
        return;
    }
    if (!reply.snapshot)
        throw std::runtime_error("History reply omitted a page snapshot");
    document_.completeHistoryRequest(reply.page_id, std::move(*reply.snapshot), reply.message);
}
void LiveConnection::persistIdentity() {
    if (!attachment_)
        throw std::runtime_error("Cannot persist an unbound session");
    if (reconnect_only_) {
        finishSynchronization();
        return;
    }
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
    verified_entry_ = WorkspaceEntry{endpoint_,         attachment_->identity, fingerprint_,
                                     document_.title(), document_.directory(), agent_};
    expected_identity_ = attachment_->identity;
    document_.setSessionId(QString::fromLatin1(attachment_->identity.session_id.toHex()));
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
    case wire::Kind::attention_snapshot: {
        auto snapshot = wire::decode_attention_snapshot(frame.payload);
        if (!ready_ || !attachment_ || snapshot.attachment != *attachment_)
            throw std::runtime_error("Stale attention attachment");
        document_.applyAttention(std::move(snapshot));
        return;
    }
    case wire::Kind::attention_retry: {
        const auto control = wire::decode_control(frame.payload);
        if (!ready_ || !attachment_ || control.attachment != *attachment_)
            throw std::runtime_error("Stale decision rejection attachment");
        document_.retryAttention(wire::decode_attention_decision(control.payload));
        return;
    }
    case wire::Kind::history_page:
        acceptHistoryReply(wire::decode_history_reply(frame.payload));
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
