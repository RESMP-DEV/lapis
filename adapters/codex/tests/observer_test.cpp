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

constexpr int maximum_test_wait_ms = 5'000;
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
        });
    }

    void start() { require(root.isValid() && server.listen(path()), "fake source listen failed"); }

    [[nodiscard]] QString path() const { return root.filePath("socket"); }
    void disconnect_client() {
        require(connection != nullptr, "fake source has no connected client");
        connection->disconnectFromServer();
    }

    void send(const QJsonObject& message) {
        require(connection && connection->state() == QLocalSocket::ConnectedState,
                "fake source client disconnected");
        connection->write(frame(json(message)));
    }

    QString thread_name{QStringLiteral("thread")};
    std::vector<QJsonObject> received;
    bool resolved_before_replay{};
    int replay_large_requests{};
    std::vector<QJsonObject> replay_requests;

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
            auto temporary_request = approval(std::numeric_limits<std::int64_t>::max());
            auto temporary_params = temporary_request.value("params").toObject();
            temporary_params.insert("threadId", "temporary");
            temporary_request.insert("params", temporary_params);
            send(temporary_request);
            send({{"method", "thread/started"},
                  {"params", QJsonObject{{"thread", QJsonObject{{"id", "temporary"},
                                                                {"ephemeral", true}}}}}});
        } else if (method == QLatin1String("thread/loaded/list")) {
            send(
                QJsonObject{{QStringLiteral("id"), id},
                            {QStringLiteral("result"),
                             QJsonObject{{QStringLiteral("data"),
                                          QJsonArray{QStringLiteral("temporary"), thread_name}}}}});
        } else if (method == QLatin1String("thread/resume") && !no_rollout_returned) {
            no_rollout_returned = true;
            send(QJsonObject{
                {QStringLiteral("id"), id},
                {QStringLiteral("error"),
                 QJsonObject{{QStringLiteral("code"), -32600},
                             {QStringLiteral("message"),
                              QStringLiteral("no rollout found for thread id ") + thread_name}}}});
        } else if (method == QLatin1String("thread/resume")) {
            send(QJsonObject{{QStringLiteral("id"), id},
                             {QStringLiteral("result"),
                              QJsonObject{{QStringLiteral("thread"),
                                           QJsonObject{{QStringLiteral("id"), thread_name},
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
                       QJsonObject{{"threadId", thread_name},
                                   {"requestId", std::numeric_limits<std::int64_t>::max()}}}});
            send(approval(std::numeric_limits<std::int64_t>::max()));
            for (const auto& replay_request : replay_requests)
                send(replay_request);
            for (int index = 0; index < replay_large_requests; ++index) {
                auto replay_request = approval(index);
                auto params = replay_request.value("params").toObject();
                params.insert("command", QString(15000, 'x'));
                replay_request.insert("params", params);
                send(replay_request);
            }
            send(QJsonObject{{QStringLiteral("id"), id},
                             {QStringLiteral("result"),
                              QJsonObject{{QStringLiteral("thread"),
                                           QJsonObject{{QStringLiteral("id"), thread_name},
                                                       {"ephemeral", false}}}}}});
        }
    }

    [[nodiscard]] QJsonObject approval(std::int64_t identifier) const {
        return QJsonObject{
            {QStringLiteral("id"), identifier},
            {QStringLiteral("method"), QStringLiteral("item/commandExecution/requestApproval")},
            {QStringLiteral("params"),
             QJsonObject{{QStringLiteral("threadId"), thread_name},
                         {QStringLiteral("turnId"), QStringLiteral("turn")},
                         {QStringLiteral("itemId"), QStringLiteral("item")},
                         {QStringLiteral("command"), QStringLiteral("echo fixture")},
                         {QStringLiteral("availableDecisions"),
                          QJsonArray{QStringLiteral("accept"), QStringLiteral("decline"),
                                     QStringLiteral("cancel")}}}}};
    }

    // Unix transport fixture: a short path stays within macOS sockaddr_un limits.
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

void reconnect_during_initialization_and_start_new_source() {
    State state{"lifecycle", "codex"};
    Source first;
    first.start();
    Observer observer{state};
    int initializations = 0;
    QObject::connect(&observer, &Observer::initialized, [&] {
        if (++initializations == 1)
            observer.reconnect();
    });
    observer.start(first.path(), Observer::qualifiedBinarySha256());
    require(wait_for([&] { return state.ready(); }), "reentrant initialization reconciles");
    require(initializations == 2, "replacement transport initialized once");
    require(std::count_if(first.received.begin(), first.received.end(),
                          [](const auto& message) {
                              return message.value("method") == "thread/loaded/list";
                          }) == 1,
            "old initialization must not discover on the replacement transport");
    observer.stop();
    require(!state.connected() &&
                !observer.details(std::numeric_limits<std::int64_t>::max()).isEmpty(),
            "stop preserves stale request details");
    Source second;
    second.thread_name = "second-thread";
    second.start();
    observer.start(second.path(), Observer::qualifiedBinarySha256());
    require(wait_for([&] { return state.ready(); }), "fresh source start reconciles");
    require(observer.threadId() == second.thread_name &&
                state.pending().begin()->second.request.thread_id == "second-thread",
            "fresh start must not retain the previous thread binding");
}

QJsonObject large_approval(std::int64_t id) {
    return {{"id", id},
            {"method", "item/commandExecution/requestApproval"},
            {"params", QJsonObject{{"threadId", "thread"},
                                   {"turnId", "turn"},
                                   {"itemId", "large"},
                                   {"command", QString(15000, 'x')},
                                   {"availableDecisions", QJsonArray{"accept", "decline"}}}}};
}

void pending_details_budget() {
    State state{"budget", "codex"};
    Source source;
    source.start();
    Observer observer{state};
    observer.start(source.path(), Observer::qualifiedBinarySha256());
    require(wait_for([&] { return state.ready(); }), "budget fixture ready");
    for (std::int64_t id = 0; id < 32; ++id)
        source.send(large_approval(id));
    require(wait_for([&] { return state.pending().size() == 33; }), "bounded requests accepted");
    for (int duplicate = 0; duplicate < 8; ++duplicate)
        source.send(large_approval(0));
    source.send({{"method", "serverRequest/resolved"},
                 {"params", QJsonObject{{"threadId", "thread"}, {"requestId", 0}}}});
    require(wait_for([&] { return state.pending().size() == 32; }),
            "duplicates do not consume budget");
    source.send(large_approval(32));
    require(wait_for([&] { return state.pending().size() == 33; }), "resolution frees budget");
    for (std::int64_t id = 33; id < 40; ++id)
        source.send(large_approval(id));
    require(wait_for([&] { return !state.connected(); }), "aggregate overflow disconnects source");
    require(observer.diagnostic().contains("details exceeded limit") && !state.ready(),
            "overflow is explicit and disables decisions");
    qsizetype retained_bytes = 0;
    for (const auto& [id, pending] : state.pending()) {
        retained_bytes += json(observer.details(id)).size();
        require(pending.status == lapis::session::attention::RequestStatus::stale,
                "overflow retains stale evidence");
    }
    require(retained_bytes <= qsizetype{512} * 1024 && state.pending().size() >= 33,
            "retained details fit within the adapter budget");
    const auto retained_count = state.pending().size();
    source.replay_large_requests = 36;
    observer.reconnect();
    require(wait_for([&] { return !state.connected(); }), "oversized replay fails closed");
    require(state.pending().size() == retained_count,
            "failed replay preserves the prior bounded request set");
    source.replay_large_requests = 0;
    observer.reconnect();
    require(wait_for([&] { return state.ready(); }), "bounded replay recovers from overflow");
    require(state.pending().size() == 1, "authoritative replay replaces stale requests");
}

void implicit_retirement_survives_replay() {
    State state{"retirement", "codex"};
    Source source;
    source.start();
    Observer observer{state};
    observer.start(source.path(), Observer::qualifiedBinarySha256());
    require(wait_for([&] { return state.ready(); }), "retirement fixture ready");
    const auto id = std::numeric_limits<std::int64_t>::max();
    source.send(large_approval(0));
    require(wait_for([&] { return state.pending().size() == 2; }), "additional request accepted");
    const auto epoch = state.epoch();
    const auto reconcile = [&] {
        const auto revision = state.pending().at(id).revision;
        source.send({{"method", "thread/started"},
                     {"params", QJsonObject{{"thread", QJsonObject{{"id", "thread"},
                                                                   {"ephemeral", false}}}}}});
        require(
            wait_for([&] { return state.ready() && state.pending().at(id).revision != revision; }),
            "same-epoch reconciliation finished");
    };
    reconcile();
    require(state.epoch() == epoch && state.pending().size() == 1,
            "snapshot implicitly retires omitted same-epoch request");
    source.send(large_approval(0));
    // A subsequent observed activity event orders the check after the late replay.
    source.send({{"method", "turn/started"}, {"params", QJsonObject{{"threadId", "thread"}}}});
    require(
        wait_for([&] { return state.activity() == lapis::session::attention::Activity::working; }),
        "late replay processed");
    require(observer.details(std::int64_t{0}).isEmpty() && state.pending().size() == 1,
            "late replay must not resurrect retired adapter details");
    source.replay_requests = {large_approval(0)};
    reconcile();
    require(state.ready() && state.pending().size() == 1,
            "later replay snapshot does not resurrect implicitly retired IDs");
}

void request_id_and_answer_boundaries() {
    State state{"boundaries", "codex"};
    Source source;
    source.start();
    Observer observer{state};
    observer.start(source.path(), Observer::qualifiedBinarySha256());
    require(wait_for([&] { return state.ready(); }), "boundary fixture ready");
    source.send(large_approval(std::numeric_limits<std::int64_t>::min()));
    for (const std::int64_t id : {9007199254740992LL, 9007199254740993LL})
        source.send(large_approval(id));
    require(wait_for([&] { return state.pending().size() == 4; }),
            "full int64 IDs and adjacent values above double precision remain distinct");
    QJsonArray questions;
    QJsonObject oversized;
    QJsonObject corrected;
    for (int index = 0; index < 16; ++index) {
        const auto key = QString::number(index);
        questions.append(
            QJsonObject{{"id", key}, {"question", "Answer"}, {"options", QJsonValue::Null}});
        oversized.insert(key, QJsonObject{{"answers", QJsonArray{QString(8192, 'x')}}});
        corrected.insert(key, QJsonObject{{"answers", QJsonArray{"short"}}});
    }
    source.send({{"id", "answers"},
                 {"method", "item/tool/requestUserInput"},
                 {"params", QJsonObject{{"threadId", "thread"}, {"questions", questions}}}});
    const RequestId answer_id{std::string{"answers"}};
    require(wait_for([&] { return state.pending().contains(answer_id); }),
            "answer request accepted");
    const auto revision = state.pending().at(answer_id).revision;
    require(!observer.decide(state.epoch(), answer_id, revision, "submit", oversized) &&
                !state.pending().at(answer_id).submitted,
            "oversized response is rejected before consuming its token");
    require(observer.decide(state.epoch(), answer_id, revision, "submit", corrected),
            "corrected bounded answer reuses the unconsumed token");
    for (const auto invalid : {1.5, -1.5, 9223372036854775808.0, -9223372036854777856.0}) {
        auto request = large_approval(0);
        request.insert("id", invalid);
        source.send(request);
        require(wait_for([&] { return !state.connected(); }),
                "non-integral or out-of-range ID rejected");
        require(observer.diagnostic().contains("invalid identity"), "invalid ID diagnostic");
        observer.reconnect();
        require(wait_for([&] { return state.ready(); }), "reconcile after invalid source identity");
    }
}

int run(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    reconnect_during_initialization_and_start_new_source();
    pending_details_budget();
    request_id_and_answer_boundaries();
    implicit_retirement_survives_replay();
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

    require(wait_for([&] {
                return std::count_if(source.received.begin(), source.received.end(),
                                     [](const auto& item) {
                                         return item.value("method") == "thread/resume";
                                     }) >= 2;
            }),
            "no-rollout retry resumes the same thread");
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

    const auto response_count = [&] {
        return std::count_if(source.received.begin(), source.received.end(),
                             [](const auto& item) { return !item.contains("method"); });
    };
    const auto sent_responses = response_count();
    source.resolved_before_replay = true;
    observer.reconnect();
    require(wait_for([&] { return state.ready(); }), "explicit reconnect reconciles");
    require(observer.threadId() == QStringLiteral("thread"), "thread scope retained");
    require(response_count() == sent_responses, "explicit reconnect sends no decision replay");
    require(state.pending().empty(), "resolution wins over a later copied request during replay");

    for (const auto* method : {"thread/closed", "thread/archived"}) {
        source.send({{"method", method}, {"params", QJsonObject{{"threadId", "thread"}}}});
        require(wait_for([&] { return !state.connected(); }), "closed source disables replies");
        observer.reconnect();
        require(wait_for([&] { return state.ready(); }),
                "restored source reconciles after closure");
    }
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
