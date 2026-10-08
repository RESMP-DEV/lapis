#ifndef LAPIS_DESKTOP_TESTS_WIRE_FIXTURE_HPP
#define LAPIS_DESKTOP_TESTS_WIRE_FIXTURE_HPP

#include "launch_spec.hpp"
#include "live_connection.hpp"
#include "session_descriptor.hpp"
#include "transport/local_protocol.hpp"
#include "workspace.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QLocalServer>
#include <QLocalSocket>
#include <QTemporaryDir>
#include <QThread>

#include <functional>
#include <optional>
#include <source_location>
#include <stdexcept>
#include <string>

namespace lapis::desktop::tests {
namespace wire = lapis::session::wire;
using lapis::desktop::SessionPreview;

inline void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}

// The test side of the service wire. Both transport and input suites drive the
// same frame contract; suite-local assertions stay in each test.
inline void until(const std::function<bool()>& condition,
                  std::source_location where = std::source_location::current(),
                  int timeout_ms = 8000) {
    QElapsedTimer time;
    time.start();
    while (time.elapsed() < timeout_ms) {
        if (condition())
            return;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
    throw std::runtime_error("Event deadline expired at line " + std::to_string(where.line()));
}
inline void settle() {
    QElapsedTimer time;
    time.start();
    while (time.elapsed() < 50) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
}
struct WirePeer final {
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
struct WireFixture final {
    QLocalServer server;
    QTemporaryDir directory{QStringLiteral("/tmp/lapis-v4-XXXXXX")};
    lapis::session::LaunchSpec launch;
    wire::SessionIdentity identity{wire::new_id(), wire::new_id()};
    lapis::session::Terminal terminal{{4, 2}};
    QString endpoint;
    quint64 generation_{1};
    SessionPreview document{
        QStringLiteral("test"), QStringLiteral("/tmp"), {}, QColor(Qt::white), ""};
    WireFixture() {
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
    ~WireFixture() { server.close(); }
    WirePeer accept() {
        until([&] { return server.hasPendingConnections(); });
        return {server.nextPendingConnection(), {}};
    }
    wire::AttachRequest request(WirePeer& peer) {
        const auto frame = peer.read();
        require(frame.kind == wire::Kind::attach, "Expected attachment request");
        const auto request = wire::decode_attach(frame.payload);
        require(request.fingerprint == lapis::session::launch_fingerprint(launch),
                "Wrong launch fingerprint");
        return request;
    }
    void hello(WirePeer& peer, quint64 generation = 1, bool paste_transactions = false) {
        generation_ = generation;
        peer.send(wire::Kind::hello,
                  wire::encode_hello({{identity, generation}, 123, paste_transactions}));
        until([&] { return document.connectionState() == QStringLiteral("synchronizing"); });
        require(!document.inputReady(), "Hello enabled input before the screen");
    }
    void screen(WirePeer& peer, quint64 generation = 1, quint64 sequence = 1) {
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
    wire::HistoryRequest historyRequest(WirePeer& peer) {
        const auto frame = peer.read();
        require(frame.kind == wire::Kind::history_request, "Expected history request");
        const auto control = wire::decode_control(frame.payload);
        require(control.attachment == wire::Attachment{identity, generation_},
                "History request lacked current attachment");
        return wire::decode_history_request(control.payload);
    }
    void historyReply(WirePeer& peer, quint64 request_id, quint64 page_id,
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
} // namespace lapis::desktop::tests

#endif // LAPIS_DESKTOP_TESTS_WIRE_FIXTURE_HPP
