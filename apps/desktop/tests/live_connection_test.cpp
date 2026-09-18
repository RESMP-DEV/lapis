#include "session_descriptor.hpp"
#include "transport/local_protocol.hpp"
#include "workspace.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QLocalServer>
#include <QLocalSocket>
#include <QTemporaryDir>
#include <QThread>
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
    QTemporaryDir directory{QStringLiteral("/tmp/lapis-v3-XXXXXX")};
    QLocalServer server;
    lapis::session::LaunchSpec launch;
    wire::SessionIdentity identity{wire::new_id(), wire::new_id()};
    lapis::session::Terminal terminal{{4, 2}};
    QString endpoint;
    SessionPreview document{
        QStringLiteral("test"), QStringLiteral("/tmp"), {}, QColor(Qt::white), ""};
    Fixture() {
        require(directory.isValid(), "Temporary directory failed");
        endpoint = directory.filePath(QStringLiteral("session.sock"));
        launch = lapis::session::validate_launch({.program = QStringLiteral("/bin/cat"),
                                                  .arguments = {},
                                                  .directory = directory.path()});
        server.setSocketOptions(QLocalServer::UserAccessOption);
        require(server.listen(endpoint), "Fixture listener failed");
        terminal.feed("screen");
    }
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
        peer.send(wire::Kind::hello, wire::encode_hello({{identity, generation}, 123}));
        until([&] { return document.connectionState() == QStringLiteral("synchronizing"); });
        require(!document.inputReady(), "Hello enabled input before the screen");
    }
    void screen(Peer& peer, quint64 generation = 1, quint64 sequence = 1) {
        peer.send(
            wire::Kind::snapshot,
            wire::encode_snapshot_message({{identity, generation}, sequence, terminal.snapshot()}));
        until([&] { return document.inputReady(); });
        const auto ack = peer.read();
        require(ack.kind == wire::Kind::ready, "Screen was not acknowledged before input");
        const auto ready = wire::decode_ready(ack.payload);
        require(ready.attachment == wire::Attachment{identity, generation} &&
                    ready.sequence == sequence,
                "Invalid synchronization acknowledgement");
        const auto resize = peer.read();
        require(resize.kind == wire::Kind::resize, "Expected post-synchronization resize");
    }
};
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
        missing_and_replaced();
        lost_before_screen();
        legacy_server();
        std::cout << "Identity, initial-screen gating, explicit reconnect/discovery, stale "
                     "snapshot and legacy rejection passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
