#include "live_connection.hpp"
#include "app_paths.hpp"
#include "platform/posix/local_endpoint.hpp"
#include "session_descriptor.hpp"
#include "workspace.hpp"
#include <QDataStream>
#include <QDateTime>
#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QProcess>
#include <QPromise>
#include <QThreadPool>
#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace lapis::desktop {
namespace wire = session::wire;
namespace {
constexpr int codex_sync_timeout_ms = 15000;
constexpr int terminal_sync_timeout_ms = 5000;
constexpr int history_timeout_ms = 5000;
// Input waiting for the socket, and what each message adds to it: a kind, a
// length and the attachment.
constexpr qint64 input_queue_bytes = qint64{1024} * 1024;
constexpr qint64 message_overhead = 45;
bool start_service_detached(const ServiceLaunchRequest& launch) {
    QProcess service;
    service.setProgram(launch.program);
    service.setArguments(launch.arguments);
    service.setStandardOutputFile(launch.log, QIODevice::Append);
    service.setStandardErrorFile(launch.log, QIODevice::Append);
    if (!launch.environment.isEmpty()) {
        auto environment = QProcessEnvironment::systemEnvironment();
        for (auto it = launch.environment.cbegin(); it != launch.environment.cend(); ++it)
            environment.insert(it.key(), it.value());
        service.setProcessEnvironment(environment);
    }
    return service.startDetached();
}
} // namespace
SessionPreview::~SessionPreview() { live_.reset(); }
void SessionPreview::startLive(const QString& endpoint, const session::LaunchSpec& launch,
                               wire::AttachMode mode) {
    link_folder_ =
        QFileInfo(launch.program).fileName() == QLatin1String("ssh") ? QString() : launch.directory;
    live_ = std::make_unique<LiveConnection>(*this, endpoint, launch, mode);
}
void SessionPreview::applySnapshot(session::TerminalSnapshot snapshot) {
    noteOutput();
    waiting_.reset();
    live_snapshot_ = std::move(snapshot);
    live_snapshot_received_ = true;
    live_snapshot_ready_ = live();
    if (!history_active_) {
        snapshot_ = live_snapshot_;
        emit snapshotChanged();
    }
}
void SessionPreview::offerSnapshot(QByteArray encoded) {
    noteOutput();
    waiting_ = std::move(encoded);
    live_snapshot_received_ = true;
    live_snapshot_ready_ = live();
    if (viewers_.empty())
        return;
    if (*viewers_.begin() == 0) {
        decodeWaiting();
        if (!history_active_)
            emit snapshotChanged();
        return;
    }
    if (!decode_timer_.isActive()) {
        // The first eligible screen opens the window now; later screens share
        // the remaining part of the viewer interval. Replacing waiting_ keeps
        // only the newest state of a burst for that deadline.
        decode_timer_.start(static_cast<int>(remainingDecodeDelay()));
    }
}
qint64 SessionPreview::remainingDecodeDelay() const {
    // The fastest viewer's interval paces decodes, and a decode outside the
    // timer (history opening, viewer resume) still counts against it, so the
    // wait is only the remaining part. No attempt yet means no wait.
    if (viewers_.empty() || !last_decode_attempt_.isValid())
        return 0;
    const qint64 interval = *viewers_.begin();
    const qint64 since = last_decode_attempt_.elapsed();
    return since >= interval ? 0 : interval - since;
}
bool SessionPreview::decodeWaiting() const {
    if (!waiting_)
        return false;
    // Decode work itself paces the next attempt, even when that newest screen
    // is unreadable; otherwise malformed bursts would retry without a
    // deadline. Publication is separate: the timer suppresses its signal
    // while history owns the visible screen.
    last_decode_attempt_.start();
    ++decode_attempts_;
    auto encoded = std::move(*waiting_);
    waiting_.reset();
    try {
        live_snapshot_ = wire::decode_snapshot(encoded);
    } catch (const std::exception& error) {
        // Keep the last good screen; the service sent one lapis cannot read.
        qWarning().noquote() << "Screen not decoded:" << error.what();
        return false;
    }
    ++decoded_screens_;
    if (!history_active_)
        snapshot_ = live_snapshot_;
    return true;
}
void SessionPreview::addViewer(int interval_ms) {
    const bool first = viewers_.empty();
    if (first)
        last_decode_attempt_.invalidate();
    viewers_.insert(std::max(0, interval_ms));
    if (first && decodeWaiting() && !history_active_)
        emit snapshotChanged();
}
void SessionPreview::removeViewer(int interval_ms) {
    const auto found = viewers_.find(std::max(0, interval_ms));
    if (found != viewers_.end())
        viewers_.erase(found);
    if (viewers_.empty())
        decode_timer_.stop();
}
void SessionPreview::captureHistoryScreen() {
    if (!strip_) {
        decodeWaiting();
        strip_screen_ = live_snapshot_;
    }
}
void SessionPreview::beginHistoryRequest() {
    // History shows the live screen until a page comes, so take the newest.
    decodeWaiting();
    history_active_ = true;
    history_request_pending_ = true;
    emit historyChanged();
    emit connectionChanged();
}
void SessionPreview::completeHistoryRequest(quint64 page_id, session::TerminalSnapshot snapshot,
                                            const QString& message) {
    history_active_ = true;
    history_request_pending_ = false;
    history_message_ = message;
    // A service that places its pages says so with more than the page.
    const auto place = snapshot.history;
    history_scrubbable_ = place.viewport_offset > 0 || place.total_rows > place.viewport_rows;
    if (!strip_) {
        // The newest page: browsing begins at the screen as it is now, below
        // every row kept so far. A service that does not place its pages
        // says its page is all there is; older ones go on top as asked.
        decodeWaiting();
        strip_.emplace(strip_screen_ ? std::move(*strip_screen_) : live_snapshot_,
                       place.total_rows);
        strip_screen_.reset();
        strip_oldest_page_ = page_id;
        strip_->addPage(std::move(snapshot));
        strip_->moveTo(static_cast<qint64>(strip_->archived()) - std::max(1, strip_rows_asked_));
    } else if (strip_extending_) {
        strip_oldest_page_ = page_id;
        strip_->prependPage(std::move(snapshot));
        strip_->moveTo(static_cast<qint64>(strip_->top()) - std::max(1, strip_rows_asked_));
    } else {
        strip_->addPage(std::move(snapshot));
    }
    strip_rows_asked_ = 0;
    strip_extending_ = false;
    emit connectionChanged();
    showStrip();
}
void SessionPreview::showStrip() {
    if (!strip_)
        return;
    // Rows no page holds wait for their page; a service that cannot fetch
    // by row shows them blank.
    if (const auto row = strip_->missing(); row && history_scrubbable_) {
        if (!history_request_pending_ && live_)
            live_->requestHistory(wire::HistoryDirection::at, *row);
        if (history_request_pending_)
            return;
    }
    snapshot_ = strip_->view();
    emit historyChanged();
    emit snapshotChanged();
}
void SessionPreview::extendStrip() {
    if (history_request_pending_ || !live_)
        return;
    // Row 0 of a placed history is the oldest kept.
    if (history_scrubbable_ || strip_oldest_page_ == 0) {
        history_message_ = tr("The oldest kept row");
        emit historyChanged();
        return;
    }
    strip_extending_ = true;
    live_->requestHistory(wire::HistoryDirection::older, strip_oldest_page_);
}
qreal SessionPreview::historyPosition() const {
    if (!strip_ || strip_->archived() == 0)
        return 1;
    return static_cast<qreal>(strip_->top()) / static_cast<qreal>(strip_->archived());
}
qreal SessionPreview::historySpan() const {
    if (!strip_)
        return 1;
    const auto rows = static_cast<qreal>(strip_->rows());
    return rows / std::max<qreal>(1, static_cast<qreal>(strip_->archived()) + rows);
}
void SessionPreview::historyAt(qreal fraction) {
    if (!strip_ || !history_scrubbable_)
        return;
    strip_->moveTo(
        std::llround(std::clamp<qreal>(fraction, 0, 1) * static_cast<qreal>(strip_->archived())));
    history_message_.clear();
    showStrip();
}
void SessionPreview::failHistoryRequest(const QString& message) {
    if (!history_request_pending_)
        return;
    history_request_pending_ = false;
    strip_extending_ = false;
    strip_rows_asked_ = 0;
    if (!strip_)
        strip_screen_.reset();
    history_message_ = message;
    // With nothing kept yet, the live screen stays, at the size asked for
    // while the request was out.
    history_active_ = strip_.has_value();
    if (strip_) {
        snapshot_ = strip_->view();
        emit snapshotChanged();
    } else if (live_) {
        live_->sendWantedSize();
    }
    emit historyChanged();
    emit connectionChanged();
}
void SessionPreview::cancelHistoryRequests() {
    history_request_pending_ = false;
    strip_extending_ = false;
    if (!history_active_ && live_)
        live_->sendWantedSize();
    emit historyChanged();
    emit connectionChanged();
}
void SessionPreview::setHistoryRequestId(quint64 request_id) {
    if (request_id != 0)
        beginHistoryRequest();
}
void SessionPreview::olderHistory() {
    scrollHistory(static_cast<int>(strip_ ? strip_->rows() : live_snapshot_.size.rows));
}
void SessionPreview::newerHistory() {
    if (strip_)
        scrollHistory(-static_cast<int>(strip_->rows()));
}
void SessionPreview::scrollHistory(int rows) {
    if (rows == 0 || !live_)
        return;
    if (!strip_) {
        if (rows < 0)
            return; // already live
        // Rows asked for while the first page loads add up.
        strip_rows_asked_ += rows;
        if (!history_request_pending_) {
            live_->requestHistory(wire::HistoryDirection::older, 0);
        }
        return;
    }
    const auto top = static_cast<qint64>(strip_->top()) - rows;
    if (rows < 0 && top >= static_cast<qint64>(strip_->archived())) {
        returnToLive();
        return;
    }
    if (top < 0 && strip_->top() == 0) {
        strip_rows_asked_ = rows;
        extendStrip();
        return;
    }
    strip_->moveTo(top);
    history_message_.clear();
    showStrip();
}
void SessionPreview::returnToLive() {
    decodeWaiting();
    if (live_)
        live_->cancelHistoryRequest();
    strip_.reset();
    strip_screen_.reset();
    strip_rows_asked_ = 0;
    strip_oldest_page_ = 0;
    strip_extending_ = false;
    history_active_ = false;
    history_request_pending_ = false;
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
bool SessionPreview::sendText(const QByteArray& bytes, bool paste) {
    if (history_active_ || history_request_pending_ || !live_) {
        setActivity(QStringLiteral(
            "Input was not sent: no live connection or history reconciliation is pending"));
        return false;
    }
    if (paste && live_->supportsPasteTransactions())
        return live_->sendPaste(bytes) != 0;
    if (paste && bytes.size() > wire::max_input_bytes) {
        setActivity(
            QStringLiteral("This service needs a restart before it can accept large pastes"));
        return false;
    }
    return live_->send(paste ? wire::Kind::paste : wire::Kind::text, bytes);
}
quint64 SessionPreview::sendPasteAndSubmit(const QByteArray& bytes) {
    return requestPaste(bytes, true);
}
quint64 SessionPreview::requestPaste(const QByteArray& bytes, bool submit) {
    if (!live_ || history_active_ || history_request_pending_) {
        setActivity(QStringLiteral(
            "Paste was not sent: no live connection or history reconciliation is pending"));
        return 0;
    }
    return live_->sendPaste(bytes, submit);
}
bool SessionPreview::terminate() {
    return live_ && input_ready_ && live_->send(wire::Kind::terminate, {});
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
bool SessionPreview::sendWheel(int steps, int column, int row) {
    decodeWaiting();
    if (history_active_ || history_request_pending_ || !live_snapshot_.accepts_wheel ||
        steps == 0 || !live_)
        return false;
    constexpr int limit = 64;
    const int delivered = std::clamp(steps, -limit, limit);
    if (!live_->send(wire::Kind::wheel,
                     wire::encode_wheel({static_cast<qint16>(delivered),
                                         static_cast<quint16>(std::clamp(column, 0, 0xFFFF)),
                                         static_cast<quint16>(std::clamp(row, 0, 0xFFFF))})))
        return false;
    // Count only what actually went on the ordered connection, at the size
    // the service can actually receive. A rejected wheel keeps prior debt.
    program_wheel_debt_ = std::max(0, program_wheel_debt_ + delivered);
    program_wheel_cell_ = {column, row};
    return true;
}
// The same ordered connection replays forward before the next input. Keep
// trying in service-sized chunks; a full queue leaves the remaining debt for
// the next attempt.
void SessionPreview::returnProgramToBottom() {
    if (program_wheel_debt_ == 0)
        return;
    if (!live_snapshot_.alternate_screen || !live_snapshot_.accepts_wheel || history_active_ ||
        history_request_pending_)
        return;
    while (program_wheel_debt_ > 0)
    {
        const int steps = std::min(64, program_wheel_debt_);
        if (!sendWheel(-steps, program_wheel_cell_.x(), program_wheel_cell_.y()))
            return;
    }
}
void SessionPreview::claimTerminalSize() {
    if (live_ && !history_active_ && !history_request_pending_)
        live_->claimSize();
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
    if (live_)
        live_->begin(wire::AttachMode::discover);
}
void SessionPreview::startNewSession() {
    if (live_)
        live_->begin(wire::AttachMode::create);
}
void SessionPreview::setConnection(const QString& state, bool input_ready) {
    connection_state_ = state;
    if (input_ready && !input_ready_)
        ready_since_ = QDateTime::currentMSecsSinceEpoch();
    input_ready_ = input_ready;
    if (!input_ready) {
        ready_since_ = 0;
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
                               const session::LaunchSpec& launch, wire::AttachMode mode,
                               std::shared_ptr<session::DescriptorStore> descriptor_store,
                               ServiceLauncher launcher)
    : document_(document), endpoint_(std::move(endpoint)),
      service_arguments_{QStringList{endpoint_, launch.directory, launch.program} +
                         launch.arguments},
      descriptor_store_{descriptor_store ? std::move(descriptor_store)
                                         : std::make_shared<session::DescriptorStore>()},
      launcher_{std::move(launcher)}, fingerprint_(session::launch_fingerprint(launch)),
      wanted_size_(launch.size) {
    retry_.setSingleShot(true);
    retry_.setInterval(100);
    handshake_.setSingleShot(true);
    handshake_.setInterval(launch.agent == session::AgentMode::codex ? codex_sync_timeout_ms
                                                                     : terminal_sync_timeout_ms);
    // A size other than the default is the view's, so the CLI starts in it.
    if (launch.size != session::LaunchSpec{}.size)
        service_arguments_ =
            QStringList{QStringLiteral("--size"),
                        QStringLiteral("%1x%2").arg(launch.size.columns).arg(launch.size.rows)} +
            service_arguments_;
    if (launch.agent == session::AgentMode::codex)
        service_arguments_.prepend(QStringLiteral("--codex"));
    else if (launch.agent == session::AgentMode::claude)
        service_arguments_.prepend(QStringLiteral("--claude"));
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
        deferred_history_.reset();
        document_.setHistoryRequestId(0);
        document_.failHistoryRequest(QStringLiteral("History request timed out."));
        rememberCanceledHistoryRequest(request_id);
    });
    QTimer::singleShot(0, this, [this, mode] { begin(mode); });
}
LiveConnection::~LiveConnection() {
    clearDescriptorWrite();
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
    capability_retry_ = false;
    attempts_ = 0;
    last_sequence_ = 0;
    resize_sent_after_sequence_ = 0;
    shown_size_ = {};
    pending_resize_.reset();
    claimed_over_.reset();
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
        request_.hyperlinks = true;
        request_.attention_phase = true;
        request_.paste_transactions = true;
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
            const QStringList arguments =
                QStringList{QStringLiteral("--session-id"),
                            QString::fromLatin1(request_.expected.session_id.toHex())} +
                service_arguments_;
            const ServiceLaunchRequest launch_request{session_service_program(), arguments,
                                                      endpoint_ + QStringLiteral(".log"),
                                                      document_.serviceEnvironment()};
            if (!(launcher_ ? launcher_(launch_request) : start_service_detached(launch_request)))
                throw std::runtime_error("Could not start session service");
        }
        connectSocket();
    } catch (const std::exception& error) {
        fail(QString::fromUtf8(error.what()));
    }
}

void LiveConnection::invalidateHistory() {
    deferred_history_.reset();
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
    deferred_history_.reset();
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
        session::posix::widen_socket_buffers(socket_->socketDescriptor());
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
    const QPointer<LiveConnection> alive(this);
    abandonPastes(QStringLiteral("Paste delivery is unknown after disconnect; it was not retried"));
    if (!alive)
        return;
    paste_transactions_ = false;
    clearDescriptorWrite();
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
    if (payload.size() > wire::max_input_bytes ||
        socket_->bytesToWrite() + payload.size() + message_overhead > input_queue_bytes) {
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
quint64 LiveConnection::sendPaste(const QByteArray& text, bool submit) {
    if (!ready_ || failed_ || !attachment_ || !paste_transactions_) {
        report(QStringLiteral(
            "Service is not ready for confirmed paste; restart it if it is an older version"));
        return 0;
    }
    if (text.size() > wire::max_paste_bytes || pending_pastes_.size() >= 8 || next_paste_id_ == 0) {
        report(QStringLiteral("Paste size or pending request limit reached; paste was not sent"));
        return 0;
    }
    try {
        const auto id = next_paste_id_++;
        const auto frame =
            wire::frame(wire::Kind::paste_request,
                        wire::encode_paste_request({*attachment_, id, submit, text}));
        if (socket_->bytesToWrite() + frame.size() > input_queue_bytes) {
            report(QStringLiteral("Input queue full; paste was not sent"));
            return 0;
        }
        pending_pastes_.insert(id, submit);
        if (socket_->write(frame) != frame.size()) {
            fail(QStringLiteral("Paste transport failed; delivery is unknown"));
            return 0;
        }
        QTimer::singleShot(10000, this, [owner = QPointer<LiveConnection>(this), id] {
            auto* self = owner.data();
            if (self == nullptr)
                return;
            const auto found = self->pending_pastes_.find(id);
            if (found == self->pending_pastes_.end())
                return;
            const bool submitted = found.value();
            self->pending_pastes_.erase(found);
            const auto message = QStringLiteral(
                "Paste admission timed out; delivery is unknown and was not retried");
            self->report(message);
            if (owner)
                emit owner->document_.pasteResult(id, false, submitted, message);
        });
        return id;
    } catch (const std::exception& error) {
        fail(QString::fromUtf8(error.what()));
        return 0;
    }
}
void LiveConnection::abandonPastes(const QString& reason) {
    const QPointer<LiveConnection> alive(this);
    const auto pending = std::exchange(pending_pastes_, {});
    for (auto it = pending.cbegin(); it != pending.cend(); ++it) {
        if (!alive)
            return;
        emit document_.pasteResult(it.key(), false, it.value(), reason);
    }
}
void LiveConnection::resize(session::TerminalSize size) {
    wanted_size_requested_ = true;
    // Asked again for the size it wants: sent only when the terminal has
    // another. An unconfirmed resize can be superseded by another view;
    // allow one explicit re-ask after each newly received snapshot. Progress
    // is not acknowledgement: pending history still needs a matching size.
    if (size == wanted_size_ &&
        (!ready_ || size == shown_size_ ||
         (pending_resize_ == size && last_sequence_ == resize_sent_after_sequence_)))
        return;
    // Taking it back from another size is that size's claim.
    if (size == wanted_size_)
        claimed_over_ = shown_size_;
    wanted_size_ = size;
    if (!ready_)
        return;
    sendResize(size);
}

void LiveConnection::sendResize(session::TerminalSize size) {
    QByteArray bytes;
    QDataStream out(&bytes, QIODevice::WriteOnly);
    out << quint16(size.columns) << quint16(size.rows);
    if (send(wire::Kind::resize, bytes)) {
        resize_sent_after_sequence_ = last_sequence_;
        pending_resize_ = size != shown_size_ ? std::optional(size) : std::nullopt;
    }
}

void LiveConnection::claimSize() {
    if (!ready_ || !wanted_size_requested_ || shown_size_ == wanted_size_ ||
        claimed_over_ == shown_size_)
        return;
    claimed_over_ = shown_size_;
    sendResize(wanted_size_);
}

void LiveConnection::setWantedSize(session::TerminalSize size) {
    wanted_size_requested_ = true;
    wanted_size_ = size;
}

void LiveConnection::sendWantedSize() {
    if (ready_ && wanted_size_requested_ && wanted_size_ != shown_size_ &&
        (pending_resize_ != wanted_size_ || last_sequence_ != resize_sent_after_sequence_))
        sendResize(wanted_size_);
}

void LiveConnection::applyWantedSize() {
    // Canceling a failed history request may already have queued this size.
    // Returning to live shares that send until another snapshot arrives.
    if (pending_resize_ == wanted_size_ && resize_sent_after_sequence_ == last_sequence_)
        return;
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
    const wire::HistoryRequest request{request_id, reference, direction};
    if (pending_resize_) {
        deferred_history_ = request;
        return;
    }
    queueHistoryRequest(request);
}
void LiveConnection::queueHistoryRequest(const wire::HistoryRequest& request) {
    // The frozen screen must match the size the service used for its archive.
    document_.captureHistoryScreen();
    if (!send(wire::Kind::history_request, wire::encode_history_request(request))) {
        history_timeout_.stop();
        outstanding_history_request_.reset();
        deferred_history_.reset();
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
    if (hello.paste_transactions && !request_.paste_transactions)
        throw std::runtime_error("Unrequested paste capability");
    attachment_ = hello.attachment;
    paste_transactions_ = hello.paste_transactions;
    document_.setServiceIdentity(attachment_->identity.session_id);
    document_.setConnection(QStringLiteral("synchronizing"), false);
    report(QStringLiteral("Restoring terminal screen"));
    qInfo() << "Connected terminal PID" << hello.pid;
}
void LiveConnection::acceptSnapshot(wire::SnapshotEnvelope message) {
    if (!attachment_ || message.attachment != *attachment_ || message.sequence <= last_sequence_)
        throw std::runtime_error("Stale or mismatched terminal snapshot");
    const bool initial = last_sequence_ == 0;
    last_sequence_ = message.sequence;
    // Until a view asks for a size, keep the service's current one rather than
    // resizing a reattached agent to the launch default.
    if (initial && !wanted_size_requested_)
        wanted_size_ = message.size;
    if (message.size != shown_size_) {
        shown_size_ = message.size;
        claimed_over_.reset();
    }
    document_.setSnapshotTiming(
        {{QStringLiteral("sequence"), QVariant::fromValue(message.sequence)},
         {QStringLiteral("pty_read_ns"), QVariant::fromValue(message.timing.pty_read_ns)},
         {QStringLiteral("parse_end_ns"), QVariant::fromValue(message.timing.parse_end_ns)},
         {QStringLiteral("publish_ns"), QVariant::fromValue(message.timing.publish_ns)}});
    // Decoded when a view shows it (most of a millisecond for a whole screen).
    document_.offerSnapshot(std::move(message.encoded));
    if (pending_resize_ == shown_size_) {
        pending_resize_.reset();
        if (deferred_history_) {
            const auto request = *deferred_history_;
            deferred_history_.reset();
            queueHistoryRequest(request);
        }
    }
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
    const auto expected = *attachment_;
    const auto sequence = last_sequence_;
    clearDescriptorWrite();
    descriptor_ticket_ = descriptor_store_->prepare(endpoint_, fingerprint_, expected.identity);
    const auto ticket = descriptor_ticket_;
    descriptor_write_ = std::make_unique<QFutureWatcher<QString>>();
    auto* watcher = descriptor_write_.get();
    connect(watcher, &QFutureWatcher<QString>::finished, this,
            [this, watcher, expected, sequence, ticket] {
                if (descriptor_ticket_ == ticket)
                    descriptor_ticket_.reset();
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
    QThreadPool::globalInstance()->start(
        [promise = std::move(promise), store = descriptor_store_, ticket]() mutable {
            promise.start();
            QString error;
            try {
                ticket->stage();
                error = store->commit(*ticket);
            } catch (const std::exception& failure) {
                error = QString::fromUtf8(failure.what());
            }
            promise.addResult(error);
            promise.finish();
        });
}
void LiveConnection::clearDescriptorWrite() {
    if (descriptor_ticket_) {
        descriptor_ticket_->cancel();
        descriptor_ticket_.reset();
    }
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
        acceptSnapshot(wire::decode_snapshot_envelope(frame.payload));
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
    case wire::Kind::paste_result: {
        const auto result = wire::decode_paste_result(frame.payload);
        if (!ready_ || !attachment_ || result.attachment != *attachment_)
            throw std::runtime_error("Stale paste receipt");
        const auto found = pending_pastes_.find(result.request_id);
        if (found == pending_pastes_.end())
            return; // A late receipt after declared uncertainty never replays input.
        const bool submitted = found.value();
        pending_pastes_.erase(found);
        if (!result.queued)
            report(result.message);
        emit document_.pasteResult(result.request_id, result.queued, submitted, result.message);
        return;
    }
    case wire::Kind::status: {
        const auto status = wire::decode_status(frame.payload);
        // Downgrade paste first, then phase, retaining links where supported,
        // then links for older v6 services. Both retries preserve identity and
        // reconnect the socket only; neither path launches a service.
        if (!attachment_ &&
            (request_.paste_transactions || request_.attention_phase || request_.hyperlinks) &&
            status.code == wire::StatusCode::rejected &&
            status.message == QStringLiteral("Invalid local session message")) {
            if (request_.paste_transactions)
                request_.paste_transactions = false;
            else if (request_.attention_phase)
                request_.attention_phase = false;
            else
                request_.hyperlinks = false;
            capability_retry_ = true;
            connected_ = false;
            handshake_.stop();
            disconnect(socket_.get(), nullptr, this, nullptr);
            socket_->abort();
            QTimer::singleShot(0, this, [this] {
                capability_retry_ = false;
                connectSocket();
            });
            return;
        }
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
        while (!failed_ && !capability_retry_ && wire::take_frame(buffer_, consumed, frame))
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
