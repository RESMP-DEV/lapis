#include "unix_websocket.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QLocalServer>
#include <QLocalSocket>
#include <QTemporaryDir>
#include <QThread>
#include <iostream>
#include <optional>
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
struct ClientHeader {
    qsizetype offset{};
    qsizetype length{};
};
std::optional<ClientHeader> client_header(const QByteArray& frame) {
    if (frame.size() < 2)
        return std::nullopt;
    require((static_cast<quint8>(frame[1]) & 128U) != 0);
    quint64 length = static_cast<quint8>(frame[1]) & 127U;
    const qsizetype extended = length == 126 ? 2 : length == 127 ? 8 : 0;
    const qsizetype offset = 2 + extended + 4;
    if (frame.size() < offset)
        return std::nullopt;
    if (extended != 0) {
        length = 0;
        for (qsizetype index = 2; index < 2 + extended; ++index)
            length = (length << 8U) | static_cast<quint8>(frame[index]);
    }
    require(length <= static_cast<quint64>(lapis::codex::UnixWebSocket::maximum_message_bytes));
    return ClientHeader{offset, static_cast<qsizetype>(length)};
}
struct Fixture {
    QTemporaryDir root{QStringLiteral("/tmp/lapis-ws-XXXXXX")};
    QLocalServer server;
    QLocalSocket* peer{}; // Owned by server.
    lapis::codex::UnixWebSocket client;
    int opened{};
    int failed{};
    QString failure;
    QList<QByteArray> messages;
    Fixture() {
        require(root.isValid() && server.listen(root.filePath("socket")));
        QObject::connect(&client, &lapis::codex::UnixWebSocket::opened, &server, [&] { ++opened; });
        QObject::connect(&client, &lapis::codex::UnixWebSocket::failed, &server,
                         [&](const QString& reason) {
                             ++failed;
                             failure = reason;
                         });
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
    void require_failed() {
        wait([&] { return failed == 1; });
        require(failed == 1 && messages.empty());
    }
    void write(const QByteArray& bytes) { require(peer->write(bytes) == bytes.size()); }
    QByteArray client_frame() {
        QByteArray bytes;
        wait([&] {
            bytes += peer->readAll();
            const auto header = client_header(bytes);
            return header && bytes.size() >= header->offset + header->length;
        });
        return bytes;
    }
};
QByteArray unmask(const QByteArray& frame) {
    const auto header = client_header(frame);
    if (!header)
        throw std::runtime_error("Incomplete masked client frame");
    require(header->offset + header->length == frame.size());
    QByteArray result(header->length, '\0');
    for (qsizetype index = 0; index < header->length; ++index)
        result[index] =
            static_cast<char>(static_cast<quint8>(frame[header->offset + index]) ^
                              static_cast<quint8>(frame[header->offset - 4 + index % 4]));
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
    for (const qsizetype length : {125, 126, 65535, 65536}) {
        const QByteArray payload(length, 'x');
        require(f.client.send(payload));
        require(unmask(f.client_frame()) == payload);
    }
    f.client.close();
    QCoreApplication::processEvents();
    require(f.failed == 0);
}
void invalid_frames() {
    for (const auto& bytes :
         {"8101ff", "808000000000", "8000", "010161810162", "0900", "897e007e",
          "827f0000000000000000", "817f8000000000000000", "817f0000000000100001", "817e000161",
          "c100", "8200", "8801e8", "880203ed", "880303e8ff", "880203f7", "88020bb7", "88021388"}) {
        Fixture f;
        f.upgrade();
        f.write(QByteArray::fromHex(bytes));
        f.require_failed();
    }
    Fixture f;
    static_cast<void>(f.connect());
    f.write("HTTP/1.1 101 OK\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
            "Sec-WebSocket-Accept: wrong\r\n\r\n");
    wait([&] { return f.failed == 1; });
    require(f.opened == 0);
}
void frame_budget_reschedule() {
    Fixture f;
    f.upgrade();
    QByteArray burst;
    for (int index = 0; index < 200; ++index)
        burst += QByteArray::fromHex("810161");
    f.write(burst);
    wait([&] { return f.messages.size() == 200; });
    require(f.failed == 0);
    require(f.messages == QList<QByteArray>(200, QByteArray(1, 'a')));
}
void oversized_unterminated_handshake() {
    Fixture f;
    static_cast<void>(f.connect());
    f.write("HTTP/1.1 101 OK\r\nX-Pad: " + QByteArray(17'000, 'a'));
    f.require_failed();
    require(f.opened == 0);
}
void peer_close() {
    for (const auto code :
         {1000, 1001, 1002, 1003, 1007, 1008, 1009, 1010, 1011, 1012, 1013, 1014, 3000, 4999}) {
        Fixture f;
        f.upgrade();
        f.write(QByteArray::fromHex("8802") +
                QByteArray(1, static_cast<char>(static_cast<unsigned>(code) >> 8U)) +
                QByteArray(1, static_cast<char>(static_cast<unsigned>(code) & 0xffU)));
        const auto response = f.client_frame();
        require(static_cast<quint8>(response[0]) == 0x88);
        const auto echoed_code = static_cast<unsigned>(
            static_cast<quint32>(static_cast<quint8>(unmask(response)[0])) << 8U |
            static_cast<quint8>(unmask(response)[1]));
        require(echoed_code == static_cast<unsigned>(code));
        f.require_failed();
        require(f.failure == QStringLiteral("WebSocket peer closed"));
    }

    Fixture f;
    f.upgrade();
    f.write(QByteArray::fromHex("8805") + QByteArray::fromHex("03e8e697a5"));
    f.require_failed();
    require(f.failure == QStringLiteral("WebSocket peer closed"));
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
        frame_budget_reschedule();
        oversized_unterminated_handshake();
        peer_close();
        reentrant_close();
        std::cout << "Unix WebSocket framing, bounds, handshakes, bursts and lifecycle passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
