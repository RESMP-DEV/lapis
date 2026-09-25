#include "live_connection.hpp"
#include "session_descriptor.hpp"
#include "transport/local_protocol.hpp"
#include "workspace.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QLocalServer>
#include <QLocalSocket>
#include <QTemporaryDir>
#include <QThread>
#include <array>
#include <functional>
#include <iostream>
#include <stdexcept>

namespace {
namespace wire = lapis::session::wire;
using lapis::desktop::SessionPreview;
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
void until(const std::function<bool()>& condition) {
    QElapsedTimer time;
    time.start();
    while (time.elapsed() < 8000) {
        if (condition())
            return;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
    throw std::runtime_error("Event deadline expired");
}
void settle() {
    QElapsedTimer time;
    time.start();
    while (time.elapsed() < 50) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
}
struct Peer {
    QLocalSocket* socket{}; // QLocalServer owns accepted sockets.
    QByteArray bytes;
    wire::Frame read() {
        wire::Frame frame;
        bool received = false;
        until([&] {
            bytes += socket->readAll();
            received = wire::take_frame(bytes, frame);
            return received;
        });
        return frame;
    }
    void send(wire::Kind kind, const QByteArray& payload) {
        const auto encoded = wire::frame(kind, payload);
        require(socket->write(encoded) == encoded.size(), "Could not send fixture frame");
        socket->flush();
    }
};
struct Fixture {
    QLocalServer server;
    QTemporaryDir directory{QStringLiteral("/tmp/lapis-v4-XXXXXX")};
    lapis::session::LaunchSpec launch;
    wire::SessionIdentity identity{wire::new_id(), wire::new_id()};
    lapis::session::Terminal terminal{{4, 2}};
    QString endpoint;
    quint64 generation_{1};
    SessionPreview document{
        QStringLiteral("test"), QStringLiteral("/tmp"), {}, QColor(Qt::white), ""};
    Fixture() {
        require(directory.isValid(), "Temporary directory failed");
        const QString canonicalDirectory = QFileInfo(directory.path()).canonicalFilePath();
        require(!canonicalDirectory.isEmpty(), "Temporary directory canonical path failed");
        endpoint = QDir(canonicalDirectory).absoluteFilePath(QStringLiteral("session.sock"));
        launch = lapis::session::validate_launch({.program = QStringLiteral("/bin/cat"),
                                                  .arguments = {},
                                                  .directory = directory.path()});
        server.setSocketOptions(QLocalServer::UserAccessOption);
        require(server.listen(endpoint), "Fixture listener failed");
        terminal.feed("screen");
    }
    ~Fixture() { server.close(); }
    Peer accept() {
        until([&] { return server.hasPendingConnections(); });
        return {server.nextPendingConnection(), {}};
    }
    wire::AttachRequest request(Peer& peer) {
        const auto frame = peer.read();
        require(frame.kind == wire::Kind::attach, "Expected attachment request");
        const auto request = wire::decode_attach(frame.payload);
        require(request.fingerprint == lapis::session::launch_fingerprint(launch),
                "Wrong launch fingerprint");
        return request;
    }
    void hello(Peer& peer, quint64 generation = 1) {
        generation_ = generation;
        peer.send(wire::Kind::hello, wire::encode_hello({{identity, generation}, 123}));
        until([&] { return document.connectionState() == QStringLiteral("synchronizing"); });
        require(!document.inputReady(), "Hello enabled input before the screen");
    }
    void screen(Peer& peer, quint64 generation = 1, quint64 sequence = 1) {
        peer.send(
            wire::Kind::snapshot,
            wire::encode_snapshot_message({{identity, generation}, sequence, terminal.snapshot()}));
        until([&] { return document.connectionState() == QStringLiteral("ready"); });
        const auto ack = peer.read();
        require(ack.kind == wire::Kind::ready, "Screen was not acknowledged before input");
        const auto ready = wire::decode_ready(ack.payload);
        require(ready.attachment == wire::Attachment{identity, generation} &&
                    ready.sequence == sequence,
                "Invalid synchronization acknowledgement");
        const auto resize = peer.read();
        require(resize.kind == wire::Kind::resize, "Expected post-synchronization resize");
    }
    wire::HistoryRequest historyRequest(Peer& peer) {
        const auto frame = peer.read();
        require(frame.kind == wire::Kind::history_request, "Expected history request");
        const auto control = wire::decode_control(frame.payload);
        require(control.attachment == wire::Attachment{identity, generation_},
                "History request lacked current attachment");
        return wire::decode_history_request(control.payload);
    }
    void historyReply(Peer& peer, quint64 request_id, quint64 page_id,
                      const lapis::session::TerminalSnapshot& snapshot = {},
                      const QString& message = {}, const wire::Attachment& attachment = {}) {
        peer.send(wire::Kind::history_page,
                  wire::encode_history_reply(
                      {attachment == wire::Attachment{} ? wire::Attachment{identity, generation_}
                                                        : attachment,
                       request_id, page_id, message,
                       page_id == 0 ? std::nullopt : std::optional(snapshot)}));
    }
};

void history_browsing_and_input_gating() {
    Fixture f;
    f.document.startLive(f.endpoint, f.launch, wire::AttachMode::discover);
    auto peer = f.accept();
    static_cast<void>(f.request(peer));
    f.hello(peer);
    f.screen(peer);

    f.document.olderHistory();
    const auto first = f.historyRequest(peer);
    require(first.request_id == 1 && first.reference == 0 &&
                first.direction == wire::HistoryDirection::older,
            "Invalid initial older-history request");
    require(f.document.historyActive() && f.document.historyRequestPending() &&
                !f.document.inputReady(),
            "History request did not enter bounded pending state");
    f.document.sendText("no-history-text", true);
    f.document.sendKey(lapis::session::TerminalKey::enter, {});
    f.document.resizeTerminal({7, 3});
    settle();
    require(peer.socket->bytesAvailable() == 0, "History mode sent input or resize");

    lapis::session::Terminal historical{{4, 2}};
    historical.feed("old");
    f.historyReply(peer, first.request_id, 41, historical.snapshot());
    until([&] { return !f.document.historyRequestPending(); });
    require(wire::encode_snapshot(f.document.snapshot()) ==
                wire::encode_snapshot(historical.snapshot()),
            "History page did not replace the visible page");

    f.terminal.feed("newer-live");
    peer.send(wire::Kind::snapshot,
              wire::encode_snapshot_message({{f.identity, 1}, 2, f.terminal.snapshot()}));
    until([&] {
        return f.document.snapshotTiming().value(QStringLiteral("sequence")).toULongLong() == 2;
    });
    require(wire::encode_snapshot(f.document.snapshot()) ==
                wire::encode_snapshot(historical.snapshot()),
            "Live snapshot replaced the visible history page");
    f.document.sendText("still-blocked");
    f.document.sendKey(lapis::session::TerminalKey::enter, {});
    f.document.resizeTerminal({8, 4});
    settle();
    require(peer.socket->bytesAvailable() == 0, "Displayed history allowed input or resize");

    f.document.newerHistory();
    const auto second = f.historyRequest(peer);
    require(second.request_id != first.request_id && second.reference == 41 &&
                second.direction == wire::HistoryDirection::newer,
            "Invalid newer-history request");
    f.document.returnToLive();
    require(!f.document.historyActive() && !f.document.historyRequestPending(),
            "Return to live did not clear history state");
    require(wire::encode_snapshot(f.document.snapshot()) ==
                wire::encode_snapshot(f.terminal.snapshot()),
            "Return to live did not restore the retained live snapshot");
    const auto resize = peer.read();
    require(resize.kind == wire::Kind::resize, "Deferred desired size was not restored");
    f.historyReply(peer, second.request_id, 42, historical.snapshot());
    settle();
    require(wire::encode_snapshot(f.document.snapshot()) ==
                wire::encode_snapshot(f.terminal.snapshot()),
            "Canceled history reply replaced the live screen");
    require(f.document.connectionState() == QStringLiteral("ready"),
            "Canceled history reply disconnected a usable session");
}

void unqueued_history_is_rejected_immediately() {
    Fixture f;
    lapis::desktop::LiveConnection connection(f.document, f.endpoint, f.launch,
                                              wire::AttachMode::discover);
    auto peer = f.accept();
    static_cast<void>(f.request(peer));
    f.hello(peer);
    f.screen(peer);
    // Keep the event loop still while filling the actual socket write queue.
    // Large chunks approach the bound; small ones leave no room for a request.
    size_t queued = 0;
    for (int index = 0; index < 32 && connection.send(wire::Kind::text, QByteArray(65536, 'x'));
         ++index)
        ++queued;
    bool full = false;
    for (int index = 0; index < 2048; ++index) {
        if (!connection.send(wire::Kind::text, "x")) {
            full = true;
            break;
        }
        ++queued;
    }
    require(full, "Fixture did not fill the socket queue");
    f.document.beginHistoryRequest();
    connection.requestHistory(wire::HistoryDirection::older, 0);
    require(!f.document.historyRequestPending() &&
                f.document.historyMessage().contains("could not be queued"),
            "Unqueued history remained pending until timeout");
    for (size_t index = 0; index < queued; ++index)
        require(peer.read().kind == wire::Kind::text,
                "Rejected history request reached the socket");
    settle();
    f.document.beginHistoryRequest();
    connection.requestHistory(wire::HistoryDirection::older, 0);
    const auto retried = f.historyRequest(peer);
    require(f.document.historyRequestPending(), "History could not retry after drain");
    f.historyReply(peer, retried.request_id, 0, {}, "No older history");
    until([&] { return !f.document.historyRequestPending(); });
    require(f.document.historyMessage() == "No older history", "Retried history response was lost");
}

void stale_reconnect_and_history_errors() {
    Fixture f;
    f.document.startLive(f.endpoint, f.launch, wire::AttachMode::discover);
    auto peer = f.accept();
    static_cast<void>(f.request(peer));
    f.hello(peer);
    f.screen(peer);
    f.document.olderHistory();
    const auto request = f.historyRequest(peer);
    peer.socket->abort();
    until([&] { return f.document.connectionState() == QStringLiteral("disconnected"); });
    require(!f.document.inputReady() && !f.document.historyRequestPending(),
            "Disconnect did not invalidate pending history");
    f.document.reconnect();
    auto next = f.accept();
    const auto attach = f.request(next);
    require(attach.mode == wire::AttachMode::reconnect && attach.expected == f.identity,
            "Reconnect forgot the verified identity");
    f.hello(next, 2);
    f.screen(next, 2, 2);
    lapis::session::Terminal stale{{4, 2}};
    stale.feed("stale");
    f.historyReply(next, request.request_id, 43, stale.snapshot(), {}, {f.identity, 2});
    settle();
    require(wire::encode_snapshot(f.document.snapshot()) ==
                wire::encode_snapshot(f.terminal.snapshot()),
            "Late pre-reconnect history reply replaced the screen");
    require(f.document.connectionState() == QStringLiteral("ready"),
            "Known canceled late history reply disconnected the session");

    f.document.olderHistory();
    const auto current = f.historyRequest(next);
    f.historyReply(next, current.request_id, 0, {}, QStringLiteral("No older history"));
    until([&] { return !f.document.historyRequestPending(); });
    require(f.document.historyMessage() == QStringLiteral("No older history"),
            "History message was not surfaced");
    require(f.document.connectionState() == QStringLiteral("ready") && !f.document.inputReady(),
            "No-page history reply left read-only mode");

    f.document.olderHistory();
    const auto malformed = f.historyRequest(next);
    const wire::SessionIdentity other{wire::new_id(), wire::new_id()};
    f.historyReply(next, malformed.request_id, 44, stale.snapshot(), {}, {other, 2});
    until([&] { return f.document.connectionState() == QStringLiteral("disconnected"); });
}
void handshake_and_reconnect() {
    Fixture f;
    f.document.startLive(f.endpoint, f.launch, wire::AttachMode::discover);
    auto peer = f.accept();
    require(f.request(peer).mode == wire::AttachMode::discover, "Wrong initial mode");
    f.hello(peer);
    f.document.sendText("must-not-send");
    settle();
    require(peer.socket->bytesAvailable() == 0, "Input escaped before screen readiness");
    f.screen(peer);
    f.document.sendText("accepted");
    auto input = peer.read();
    require(input.kind == wire::Kind::text, "Expected text");
    auto control = wire::decode_control(input.payload);
    require(control.attachment == wire::Attachment{f.identity, 1} && control.payload == "accepted",
            "Input lacked current attachment identity");
    const auto fingerprint = lapis::session::launch_fingerprint(f.launch);
    require(lapis::session::read_descriptor(f.endpoint, fingerprint) == f.identity,
            "Accepted identity was not persisted");
    peer.socket->abort();
    until([&] { return f.document.connectionState() == QStringLiteral("disconnected"); });
    require(!f.document.inputReady(), "Disconnected input remained enabled");
    f.document.sendText("do-not-replay");
    settle();
    require(!f.server.hasPendingConnections(), "Unexpected automatic reconnect");
    f.document.reconnect();
    auto next = f.accept();
    const auto request = f.request(next);
    require(request.mode == wire::AttachMode::reconnect && request.expected == f.identity,
            "Reconnect forgot the saved identity");
    f.hello(next, 2);
    f.screen(next, 2, 5);
    settle();
    require(next.bytes.isEmpty() && next.socket->bytesAvailable() == 0,
            "Input from old attachment was replayed");
    next.send(wire::Kind::snapshot,
              wire::encode_snapshot_message({{f.identity, 1}, 6, f.terminal.snapshot()}));
    until([&] { return !f.document.inputReady(); });
    require(lapis::session::read_descriptor(f.endpoint, fingerprint) == f.identity,
            "Stale frame changed saved identity");
}
void missing_and_replaced() {
    Fixture f;
    f.document.startLive(f.endpoint, f.launch);
    until([&] { return f.document.activity().contains(QStringLiteral("No saved session")); });
    require(!f.server.hasPendingConnections(),
            "Missing descriptor implicitly discovered a session");
    const auto fingerprint = lapis::session::launch_fingerprint(f.launch);
    const wire::SessionIdentity previous{wire::new_id(), wire::new_id()};
    lapis::session::write_descriptor(f.endpoint, fingerprint, previous);
    f.document.reconnect();
    auto peer = f.accept();
    require(f.request(peer).expected == previous, "Reconnect did not use descriptor");
    peer.send(wire::Kind::hello, wire::encode_hello({{f.identity, 1}, 123}));
    until([&] { return f.document.connectionState() == QStringLiteral("replaced"); });
    require(!f.document.inputReady(), "Replacement enabled input");
    require(lapis::session::read_descriptor(f.endpoint, fingerprint) == previous,
            "Replacement overwrote remembered identity");
    f.document.discoverSession();
    auto discovered = f.accept();
    require(f.request(discovered).mode == wire::AttachMode::discover,
            "Explicit discovery not honored");
    f.hello(discovered, 2);
    f.screen(discovered, 2, 2);
    require(lapis::session::read_descriptor(f.endpoint, fingerprint) == f.identity,
            "Discovery did not remember verified identity");
}
void attention_routing_and_reconnect() {
    using namespace lapis::session::attention;
    Fixture f;
    f.document.startLive(f.endpoint, f.launch, wire::AttachMode::discover);
    auto peer = f.accept();
    static_cast<void>(f.request(peer));
    f.hello(peer);
    f.screen(peer);
    wire::AttentionSnapshot state;
    state.attachment = {f.identity, 1};
    state.available = state.connected = state.ready = true;
    state.source_epoch = 7;
    const std::array<RequestId, 2> ids{std::int64_t{9223372036854775807LL},
                                       std::string{"9223372036854775807"}};
    for (size_t index = 0; index < ids.size(); ++index) {
        Pending pending;
        pending.request = {.id = ids[index],
                           .thread_id = "thread",
                           .turn_id = "turn",
                           .item_id = "item",
                           .reason = "Command approval",
                           .summary = "Approve",
                           .choices = {"accept", "decline"}};
        pending.source_epoch = 7;
        pending.revision = 9007199254740993ULL + index;
        state.requests.push_back({pending, {}});
    }
    const auto publish = [&] {
        peer.send(wire::Kind::attention_snapshot, wire::encode_attention_snapshot(state));
        settle();
    };
    publish();
    require(f.document.attentionCount() == 2 && f.document.attentionReady(),
            "Requests not exposed");
    const auto first_arrival = f.document.attentionSerial();
    publish();
    require(f.document.attentionSerial() == first_arrival, "Duplicate snapshot re-alerted");
    state.requests[0].pending.revision += 2;
    publish();
    require(f.document.attentionSerial() == first_arrival + 1,
            "Reused request ID with a fresh revision did not alert");
    state.source_epoch = 8;
    for (auto& request : state.requests)
        request.pending.source_epoch = 8;
    publish();
    require(f.document.attentionSerial() == first_arrival + 2,
            "Reused request ID in a fresh source epoch did not alert");
    const auto initial = f.document.attentionRequests();
    const auto first_token = initial[0].toMap().value("token").toString();
    const auto second_token = initial[1].toMap().value("token").toString();
    require(first_token != second_token, "Large revisions collapsed in UI token");
    require(!f.document.respondAttention(first_token, {{"choice", "unsupported"}}),
            "Invalid choice accepted");
    require(f.document.respondAttention(first_token, {{"choice", "decline"}}),
            "Provisional decision not sent");
    const auto rejected_frame = peer.read();
    publish();
    require(!f.document.attentionRequests()[0].toMap().value("enabled").toBool(),
            "Old snapshot unblocked provisional decision");
    peer.send(wire::Kind::attention_retry, rejected_frame.payload);
    until([&] { return f.document.attentionRequests()[0].toMap().value("enabled").toBool(); });
    require(f.document.respondAttention(first_token, {{"choice", "accept"}}),
            "Explicit decision not sent");
    const auto frame = peer.read();
    require(frame.kind == wire::Kind::attention_decision, "Wrong response kind");
    const auto envelope = wire::decode_control(frame.payload);
    const auto decision = wire::decode_attention_decision(envelope.payload);
    require(envelope.attachment == state.attachment && decision.request_id == ids[0] &&
                decision.revision == state.requests[0].pending.revision &&
                decision.source_epoch == 8 && decision.choice == "accept",
            "Decision changed exact native identity");
    require(!f.document.respondAttention(first_token, {{"choice", "accept"}}),
            "Duplicate response sent");
    publish(); // A queued older snapshot cannot re-enable the locally submitted token.
    require(!f.document.attentionRequests()[0].toMap().value("enabled").toBool(),
            "Snapshot re-enabled send");
    require(f.document.attentionCount() == 2, "Sending resolved a request");
    state.requests.erase(state.requests.begin());
    publish();
    require(f.document.attentionCount() == 1 &&
                f.document.attentionRequests()[0].toMap().value("token") == second_token,
            "Resolution removed the wrong typed ID");
    peer.socket->abort();
    until([&] { return !f.document.attentionReady(); });
    require(!f.document.respondAttention(second_token, {{"choice", "accept"}}),
            "Disconnected send accepted");
    require(f.document.attentionCount() == 1, "Disconnect discarded pending evidence");
    f.document.reconnect();
    peer = f.accept();
    static_cast<void>(f.request(peer));
    f.hello(peer, 2);
    f.screen(peer, 2);
    require(!f.document.attentionReady(), "Screen alone enabled stale attention");
    const auto before_reattach = f.document.attentionSerial();
    state.attachment.generation = 2;
    publish();
    require(f.document.attentionSerial() == before_reattach, "Attachment alone re-alerted");
    require(f.document.attentionReady(), "Reconciled requests did not become ready");
    require(!f.document.respondAttention(second_token, {{"choice", "decline"}}),
            "Old attachment token sent");
    const auto fresh = f.document.attentionRequests()[0].toMap().value("token").toString();
    require(f.document.respondAttention(fresh, {{"choice", "decline"}}),
            "Fresh attachment token failed");
    const auto reply =
        wire::decode_attention_decision(wire::decode_control(peer.read().payload).payload);
    require(reply.request_id == ids[1] && reply.choice == "decline", "String identity was coerced");
    state.attachment.generation = 1;
    publish();
    require(!f.document.inputReady(), "Wrong-attachment snapshot was accepted");
}

void lost_before_screen() {
    Fixture f;
    f.document.startLive(f.endpoint, f.launch, wire::AttachMode::discover);
    auto peer = f.accept();
    static_cast<void>(f.request(peer));
    f.hello(peer);
    peer.socket->abort();
    until([&] { return f.document.connectionState() == QStringLiteral("disconnected"); });
    require(!f.document.inputReady() && !f.document.liveSnapshotReady(),
            "Incomplete synchronization stayed ready");
    require(
        !lapis::session::read_descriptor(f.endpoint, lapis::session::launch_fingerprint(f.launch)),
        "Unrestored identity was saved");
}
void legacy_server() {
    Fixture f;
    f.document.startLive(f.endpoint, f.launch, wire::AttachMode::discover);
    auto peer = f.accept();
    static_cast<void>(f.request(peer));
    QByteArray legacy(12, '\0');
    legacy[3] = 2;
    legacy[11] = 1;
    peer.send(wire::Kind::hello, legacy);
    until([&] { return f.document.connectionState() == QStringLiteral("disconnected"); });
    require(!f.document.inputReady(), "Legacy server enabled input");
}
} // namespace
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        handshake_and_reconnect();
        history_browsing_and_input_gating();
        stale_reconnect_and_history_errors();
        unqueued_history_is_rejected_immediately();
        missing_and_replaced();
        attention_routing_and_reconnect();
        lost_before_screen();
        legacy_server();
        std::cout << "Identity, initial-screen gating, history paging/cancellation, explicit "
                     "reconnect/discovery, stale snapshot and legacy rejection passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
