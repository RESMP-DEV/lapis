#include "observer.hpp"

#include "unix_websocket.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDataStream>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using lapis::codex::Observer;
using lapis::session::attention::RequestId;
using lapis::session::attention::State;

constexpr int maximum_test_wait_ms = 2'000;
constexpr auto websocket_guid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

QByteArray json(const QJsonObject& object) {
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

QJsonObject object(const QByteArray& bytes) {
    const auto document = QJsonDocument::fromJson(bytes);
    require(document.isObject(), "expected JSON object");
    return document.object();
}

QByteArray frame(const QByteArray& payload) {
    QByteArray result;
    QDataStream stream(&result, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::BigEndian);
    stream << quint8(0x81);
    if (payload.size() < 126) {
        stream << quint8(payload.size());
    } else if (payload.size() <= 0xffff) {
        stream << quint8(126) << quint16(payload.size());
    } else {
        stream << quint8(127) << quint64(payload.size());
    }
    result.append(payload);
    return result;
}

bool decode_frame(const QByteArray& input, QByteArray& payload, qsizetype& consumed) {
    if (input.size() < 2)
        return false;
    QDataStream stream(input);
    stream.setByteOrder(QDataStream::BigEndian);
    quint8 opcode{};
    quint8 length{};
    stream >> opcode >> length;
    if ((opcode & 0x0fU) != 1)
        throw std::runtime_error("test expected a text frame");
    quint64 size = length & 0x7fU;
    qsizetype offset = 2;
    if ((length & 0x7fU) == 126) {
        if (input.size() < 4)
            return false;
        quint16 extended{};
        stream >> extended;
        size = extended;
        offset = 4;
    } else if ((length & 0x7fU) == 127) {
        if (input.size() < 10)
            return false;
        stream >> size;
        offset = 10;
    }
    const bool masked = (length & 0x80U) != 0;
    if (masked)
        offset += 4;
    if (input.size() < offset || size > static_cast<quint64>(input.size()) ||
        static_cast<quint64>(input.size() - offset) < size)
        return false;
    QByteArray unmasked(static_cast<qsizetype>(size), Qt::Uninitialized);
    for (qsizetype index = 0; index < static_cast<qsizetype>(size); ++index) {
        const auto value = static_cast<quint8>(input[offset + index]);
        const auto mask = masked ? static_cast<quint8>(input[offset - 4 + (index % 4)]) : quint8{0};
        unmasked[index] = static_cast<char>(value ^ mask);
    }
    payload = std::move(unmasked);
    consumed = offset + static_cast<qsizetype>(size);
    return true;
}

QByteArray handshake_key(const QByteArray& request) {
    const auto marker = QByteArrayLiteral("Sec-WebSocket-Key: ");
    const auto start = request.indexOf(marker);
    require(start >= 0, "WebSocket key missing");
    const auto end = request.indexOf("\r\n", start);
    require(end > start, "WebSocket key invalid");
    return request.mid(start + marker.size(), end - start - marker.size());
}

class Source final : public QObject {
  public:
    explicit Source(QObject* parent = nullptr) : QObject(parent) {
        connect(&server, &QLocalServer::newConnection, this, [this] {
            connection = server.nextPendingConnection();
            handshaken = false;
            frame_buffer.clear();
            handshake_buffer.clear();
            connect(connection, &QLocalSocket::readyRead, this, [this] { read(); });
            connect(connection, &QLocalSocket::disconnected, this, [this] { disconnected = true; });
        });
    }

    void start() { require(root.isValid() && server.listen(path()), "fake source listen failed"); }

    [[nodiscard]] QString path() const { return root.filePath("socket"); }
    void disconnect_client() { connection->disconnectFromServer(); }

    void send(const QJsonObject& message) {
        require(connection && connection->state() == QLocalSocket::ConnectedState,
                "fake source client disconnected");
        connection->write(frame(json(message)));
    }

    std::vector<QJsonObject> received;
    bool disconnected{};
    bool resolved_before_replay{};

  private:
    void read() {
        if (!handshaken) {
            handshake_buffer.append(connection->readAll());
            const auto end = handshake_buffer.indexOf("\r\n\r\n");
            if (end < 0)
                return;
            const auto request = handshake_buffer.left(end + 4);
            const auto accept = QCryptographicHash::hash(handshake_key(request) + websocket_guid,
                                                         QCryptographicHash::Sha1)
                                    .toBase64();
            connection->write(
                QByteArrayLiteral("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                                  "Connection: Upgrade\r\nSec-WebSocket-Accept: ") +
                accept + "\r\n\r\n");
            frame_buffer = handshake_buffer.mid(end + 4);
            handshake_buffer.clear();
            handshaken = true;
        } else {
            frame_buffer.append(connection->readAll());
        }
        while (!frame_buffer.isEmpty()) {
            QByteArray payload;
            qsizetype consumed{};
            if (!decode_frame(frame_buffer, payload, consumed))
                return;
            frame_buffer.remove(0, consumed);
            const auto request = object(payload);
            received.push_back(request);
            handle(request);
        }
    }

    void handle(const QJsonObject& request) {
        if (!request.contains(QStringLiteral("method")))
            return;
        const auto method = request.value(QStringLiteral("method")).toString();
        const auto id = request.value(QStringLiteral("id"));
        if (method == QLatin1String("initialize")) {
            send(
                QJsonObject{{QStringLiteral("id"), id},
                            {QStringLiteral("result"),
                             QJsonObject{{QStringLiteral("userAgent"), QStringLiteral("fixture")},
                                         {QStringLiteral("codexHome"), QStringLiteral("/fixture")},
                                         {QStringLiteral("platformFamily"), QStringLiteral("test")},
                                         {QStringLiteral("platformOs"), QStringLiteral("test")}}}});
            send(QJsonObject{{QStringLiteral("method"), QStringLiteral("initialized")}});
            send({{"method", "thread/started"},
                  {"params", QJsonObject{{"thread", QJsonObject{{"id", "temporary"},
                                                                {"ephemeral", true}}}}}});
            auto temporary_request = approval(std::numeric_limits<std::int64_t>::max());
            auto temporary_params = temporary_request.value("params").toObject();
            temporary_params.insert("threadId", "temporary");
            temporary_request.insert("params", temporary_params);
            send(temporary_request);
        } else if (method == QLatin1String("thread/loaded/list")) {
            send(QJsonObject{
                {QStringLiteral("id"), id},
                {QStringLiteral("result"),
                 QJsonObject{{QStringLiteral("data"), QJsonArray{QStringLiteral("temporary"),
                                                                 QStringLiteral("thread")}}}}});
        } else if (method == QLatin1String("thread/resume") && !no_rollout_returned) {
            no_rollout_returned = true;
            send(QJsonObject{
                {QStringLiteral("id"), id},
                {QStringLiteral("error"),
                 QJsonObject{{QStringLiteral("code"), -32600},
                             {QStringLiteral("message"),
                              QStringLiteral("no rollout found for thread id thread")}}}});
        } else if (method == QLatin1String("thread/resume")) {
            send(QJsonObject{
                {QStringLiteral("id"), id},
                {QStringLiteral("result"),
                 QJsonObject{{QStringLiteral("thread"),
                              QJsonObject{{QStringLiteral("id"), QStringLiteral("thread")},
                                          {"ephemeral", false}}}}}});
        } else if (method == QLatin1String("thread/read")) {
            if (!request.value("params").toObject().value("includeTurns").toBool()) {
                const auto thread = request.value("params").toObject().value("threadId");
                send(
                    {{"id", id},
                     {"result",
                      QJsonObject{{"thread", QJsonObject{{"id", thread},
                                                         {"ephemeral", thread == "temporary"}}}}}});
                return;
            }
            if (resolved_before_replay)
                send({{"method", "serverRequest/resolved"},
                      {"params",
                       QJsonObject{{"threadId", "thread"},
                                   {"requestId", std::numeric_limits<std::int64_t>::max()}}}});
            send(approval(std::numeric_limits<std::int64_t>::max()));
            send(QJsonObject{
                {QStringLiteral("id"), id},
                {QStringLiteral("result"),
                 QJsonObject{{QStringLiteral("thread"),
                              QJsonObject{{QStringLiteral("id"), QStringLiteral("thread")},
                                          {"ephemeral", false}}}}}});
        }
    }

    static QJsonObject approval(std::int64_t identifier) {
        return QJsonObject{
            {QStringLiteral("id"), identifier},
            {QStringLiteral("method"), QStringLiteral("item/commandExecution/requestApproval")},
            {QStringLiteral("params"),
             QJsonObject{{QStringLiteral("threadId"), QStringLiteral("thread")},
                         {QStringLiteral("turnId"), QStringLiteral("turn")},
                         {QStringLiteral("itemId"), QStringLiteral("item")},
                         {QStringLiteral("command"), QStringLiteral("echo fixture")},
                         {QStringLiteral("availableDecisions"),
                          QJsonArray{QStringLiteral("accept"), QStringLiteral("decline"),
                                     QStringLiteral("cancel")}}}}};
    }

    QTemporaryDir root{QStringLiteral("/tmp/lapis-observer-XXXXXX")};
    QLocalServer server;
    QLocalSocket* connection{};
    QByteArray handshake_buffer;
    QByteArray frame_buffer;
    bool handshaken{};
    bool no_rollout_returned{};
};

void pump(int milliseconds) {
    QEventLoop loop;
    QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
    loop.exec();
}

template <typename Predicate>
bool wait_for(Predicate predicate, int milliseconds = maximum_test_wait_ms) {
    QElapsedTimer deadline;
    deadline.start();
    while (!predicate()) {
        if (deadline.elapsed() >= milliseconds)
            return false;
        pump(20);
    }
    return true;
}

int run(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    State state{QStringLiteral("session").toStdString(), QStringLiteral("codex").toStdString()};
    Source source;
    source.start();
    Observer observer{state};
    bool initialized_signal = false;
    QObject::connect(&observer, &Observer::initialized, [&]() { initialized_signal = true; });

    observer.start(source.path(), QStringLiteral("wrong"));
    require(!initialized_signal && !state.connected(), "wrong hash must fail closed");
    require(observer.diagnostic() == QStringLiteral("Unsupported Codex binary hash"),
            "wrong-hash diagnostic");

    observer.start(source.path(), Observer::qualifiedBinarySha256());
    require(wait_for([&] { return initialized_signal; }), "initialized signal");
    require(wait_for([&] { return source.received.size() >= 2; }), "initialization messages");
    require(source.received[0].value(QStringLiteral("method")) == QStringLiteral("initialize"),
            "initialize method");
    require(source.received[0]
                .value(QStringLiteral("params"))
                .toObject()
                .value(QStringLiteral("capabilities"))
                .toObject()
                .value(QStringLiteral("experimentalApi"))
                .toBool(),
            "experimental API capability");
    require(source.received[1].value(QStringLiteral("method")) == QStringLiteral("initialized"),
            "initialized notification");

    require(wait_for([&] { return source.received.size() >= 5; }),
            "exact no-rollout retry is bounded");
    require(wait_for([&] { return state.ready() && state.pending().size() == 1; }),
            "resume/read replay");
    const RequestId identifier{std::numeric_limits<std::int64_t>::max()};
    const auto pending = state.pending().find(identifier);
    require(pending != state.pending().end(), "full signed int64 request identity");
    require(observer.details(identifier).value(QStringLiteral("command")) ==
                QStringLiteral("echo fixture"),
            "bounded approval details");
    require(!observer.decide(state.epoch(), identifier, pending->second.revision + 1,
                             QStringLiteral("accept")),
            "stale token rejected before send");
    const QString question_id = QString::number(std::numeric_limits<std::int64_t>::max());
    const RequestId question_key{question_id.toStdString()};
    source.send(
        {{"id", question_id},
         {"method", "item/tool/requestUserInput"},
         {"params",
          QJsonObject{{"threadId", "thread"},
                      {"turnId", "turn"},
                      {"itemId", "question"},
                      {"questions", QJsonArray{QJsonObject{
                                        {"id", "color"},
                                        {"header", "Color"},
                                        {"question", "Choose a color"},
                                        {"options", QJsonArray{QJsonObject{{"label", "Blue"}},
                                                               QJsonObject{{"label", "Green"}}}},
                                        {"isOther", false}}}}}}});
    require(wait_for([&] { return state.pending().size() == 2; }),
            "numeric and matching string IDs coexist");
    const auto question_revision = state.pending().at(question_key).revision;
    const QJsonObject invalid{{"color", QJsonObject{{"answers", QJsonArray{"Red"}}}}};
    require(!observer.decide(state.epoch(), question_key, question_revision, "submit", invalid),
            "invalid option must not consume token");
    const QJsonObject answer{{"color", QJsonObject{{"answers", QJsonArray{"Blue"}}}}};
    require(observer.decide(state.epoch(), question_key, question_revision, "submit", answer),
            "validated user answer is routed");
    require(wait_for([&] { return source.received.back().value("id") == question_id; }),
            "string ID preserved in response");
    require(source.received.back().value("result").toObject().value("answers").toObject() == answer,
            "answer payload matches source contract");
    source.send({{"method", "serverRequest/resolved"},
                 {"params", QJsonObject{{"threadId", "thread"}, {"requestId", question_id}}}});
    require(wait_for([&] { return state.pending().size() == 1; }) &&
                state.pending().contains(identifier),
            "resolving one simultaneous request preserves the other");
    const auto before_response = source.received.size();
    require(observer.decide(state.epoch(), identifier, pending->second.revision,
                            QStringLiteral("accept")),
            "decision token consumed");
    require(wait_for([&] { return source.received.size() == before_response + 1; }),
            "decision sent exactly once");
    const auto response = source.received.back();
    require(response.value(QStringLiteral("id")).toInteger() ==
                std::numeric_limits<std::int64_t>::max(),
            "original int64 response identity");
    require(response.value(QStringLiteral("result")).toObject().value(QStringLiteral("decision")) ==
                QStringLiteral("accept"),
            "approval response result");
    require(state.pending().at(identifier).submitted, "sending does not resolve state");
    require(!observer.decide(state.epoch(), identifier, pending->second.revision,
                             QStringLiteral("accept")),
            "token is one-shot");

    source.send(QJsonObject{
        {QStringLiteral("method"), QStringLiteral("serverRequest/resolved")},
        {QStringLiteral("params"),
         QJsonObject{{QStringLiteral("threadId"), QStringLiteral("thread")},
                     {QStringLiteral("requestId"), std::numeric_limits<std::int64_t>::max()}}}});
    require(wait_for([&] { return state.pending().empty(); }), "source resolution retires request");

    const auto sent_responses = source.received.size();
    source.resolved_before_replay = true;
    observer.reconnect();
    require(wait_for([&] { return state.ready(); }), "explicit reconnect reconciles");
    require(observer.threadId() == QStringLiteral("thread"), "thread scope retained");
    require(wait_for([&] { return source.received.size() == sent_responses + 7; }),
            "explicit reconnect sends no decision replay");
    require(state.pending().empty(), "resolution wins over a later copied request during replay");

    source.disconnect_client();
    require(wait_for([&] { return !state.connected(); }), "disconnect marks state stale");
    observer.stop();
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "observer_test: " << error.what() << '\n';
        return 1;
    }
}
