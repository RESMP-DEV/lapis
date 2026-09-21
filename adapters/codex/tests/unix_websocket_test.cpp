#include "unix_websocket.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QLocalServer>
#include <QLocalSocket>
#include <QTemporaryDir>
#include <QThread>
#include <iostream>
#include <source_location>
#include <stdexcept>

namespace {
void require(bool value, std::source_location location = std::source_location::current()) {
    if (!value)
        throw std::runtime_error("WebSocket expectation at line " +
                                 std::to_string(location.line()));
}
template <class Predicate> void wait(Predicate predicate) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < 2000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    require(predicate());
}
struct Fixture {
    QTemporaryDir root{QStringLiteral("/tmp/lapis-ws-XXXXXX")};
    QLocalServer server;
    QLocalSocket* peer{}; // Owned by server.
    lapis::codex::UnixWebSocket client;
    int opened{};
    int failed{};
    QList<QByteArray> messages;
    Fixture() {
        require(root.isValid() && server.listen(root.filePath("socket")));
        QObject::connect(&client, &lapis::codex::UnixWebSocket::opened, &server, [&] { ++opened; });
        QObject::connect(&client, &lapis::codex::UnixWebSocket::failed, &server,
                         [&](const QString&) { ++failed; });
        QObject::connect(&client, &lapis::codex::UnixWebSocket::message, &server,
                         [&](const QByteArray& text) { messages.append(text); });
    }
    QByteArray connect() {
        client.open(root.filePath("socket"));
        wait([&] { return server.hasPendingConnections(); });
        peer = server.nextPendingConnection();
        QByteArray request;
        wait([&] {
            request += peer->readAll();
            return request.contains("\r\n\r\n");
        });
        const QByteArray marker = "Sec-WebSocket-Key: ";
        const auto start = request.indexOf(marker) + marker.size();
        const auto end = request.indexOf("\r\n", start);
        require(start >= marker.size() && end > start);
        return QCryptographicHash::hash(request.mid(start, end - start) +
                                            "258EAFA5-E914-47DA-95CA-C5AB0DC85B11",
                                        QCryptographicHash::Sha1)
            .toBase64();
    }
    void upgrade() {
        const auto accept = connect();
        write("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
              "Connection: keep-alive, Upgrade\r\nSec-WebSocket-Accept: " +
              accept + "\r\n\r\n");
        wait([&] { return opened == 1; });
    }
    void write(const QByteArray& bytes) { require(peer->write(bytes) == bytes.size()); }
    QByteArray client_frame() {
        QByteArray bytes;
        wait([&] {
            bytes += peer->readAll();
            return bytes.size() >= 2 && bytes.size() >= 6 + (static_cast<quint8>(bytes[1]) & 127U);
        });
        require((static_cast<quint8>(bytes[1]) & 128U) != 0);
        return bytes;
    }
};
QByteArray unmask(const QByteArray& frame) {
    QByteArray result;
    for (qsizetype index = 6; index < frame.size(); ++index)
        result.append(static_cast<char>(static_cast<quint8>(frame[index]) ^
                                        static_cast<quint8>(frame[2 + (index - 6) % 4])));
    return result;
}
void framing() {
    Fixture f;
    require(!f.client.send("before upgrade"));
    f.upgrade();
    require(f.client.send("hello"));
    const auto frame = f.client_frame();
    require(static_cast<quint8>(frame[0]) == 0x81 && unmask(frame) == "hello");
    // Split a multibyte character across fragments, with an interleaved ping.
    f.write(QByteArray::fromHex("0102e6978901708001a5"));
    wait([&] { return f.messages.size() == 1; });
    require(f.messages.first() == QByteArray::fromHex("e697a5"));
    const auto pong = f.client_frame();
    require(static_cast<quint8>(pong[0]) == 0x8a && unmask(pong) == "p");
    require(
        !f.client.send(QByteArray(lapis::codex::UnixWebSocket::maximum_message_bytes + 1, 'x')));
    require(!f.client.send(QByteArray::fromHex("ff")));
    f.client.close();
    QCoreApplication::processEvents();
    require(f.failed == 0);
}
void invalid_frames() {
    for (const auto& bytes :
         {"8101ff", "808000000000", "8000", "010161810162", "0900", "897e007e",
          "827f0000000000000000", "817f0000000000100001", "817e000161", "c100", "8200"}) {
        Fixture f;
        f.upgrade();
        f.write(QByteArray::fromHex(bytes));
        wait([&] { return f.failed == 1; });
        require(f.messages.empty());
    }
    Fixture f;
    static_cast<void>(f.connect());
    f.write("HTTP/1.1 101 OK\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
            "Sec-WebSocket-Accept: wrong\r\n\r\n");
    wait([&] { return f.failed == 1; });
    require(f.opened == 0);
}
void reentrant_close() {
    Fixture f;
    f.upgrade();
    QObject::connect(&f.client, &lapis::codex::UnixWebSocket::message, &f.server,
                     [&](const QByteArray&) { f.client.close(); });
    f.write(QByteArray::fromHex("810161810162"));
    wait([&] { return !f.messages.empty(); });
    QCoreApplication::processEvents();
    require(f.messages.size() == 1 && f.failed == 0);
}
} // namespace
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        framing();
        invalid_frames();
        reentrant_close();
        std::cout << "Unix WebSocket framing, bounds and lifecycle passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
