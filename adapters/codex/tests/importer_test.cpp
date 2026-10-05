#include "importer.hpp"

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
#include <QPointer>
#include <QTemporaryDir>
#include <QTimer>
#include <algorithm>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {
using lapis::codex::Importer;

constexpr auto websocket_guid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

QByteArray json(const QJsonObject& value) {
    return QJsonDocument(value).toJson(QJsonDocument::Compact);
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
    if (payload.size() <= 125) {
        stream << quint8(payload.size());
    } else if (payload.size() <= std::numeric_limits<quint16>::max()) {
        stream << quint8(126) << quint16(payload.size());
    } else {
        stream << quint8(127) << quint64(payload.size());
    }
    result.append(payload);
    return result;
}

struct DecodedFrame {
    bool complete{};
    QByteArray payload;
    qsizetype consumed{};
    quint8 opcode{};
};

DecodedFrame decode_frame(const QByteArray& input) {
    DecodedFrame result;
    if (input.size() < 2)
        return result;
    QDataStream stream(input);
    stream.setByteOrder(QDataStream::BigEndian);
    quint8 marker{};
    stream >> result.opcode >> marker;
    if ((result.opcode & 0x0fU) != 1)
        throw std::runtime_error("fixture expected a text frame");
    auto length = static_cast<quint64>(marker & 0x7fU);
    qsizetype extended{};
    if (length == 126) {
        extended = 2;
    } else if (length == 127) {
        extended = 8;
    }
    if (input.size() < 2 + extended)
        return result;
    if (extended != 0) {
        length = 0;
        for (qsizetype index = 0; index < extended; ++index)
            length = (length << 8U) | static_cast<quint8>(input[2 + index]);
    }
    if (length > static_cast<quint64>(std::numeric_limits<qsizetype>::max()))
        throw std::runtime_error("fixture frame length exceeded qsizetype");
    const auto size = static_cast<qsizetype>(length);
    const bool masked = (marker & 0x80U) != 0;
    const auto mask_offset = 2 + extended;
    const auto offset = mask_offset + (masked ? 4 : 0);
    if (input.size() < offset || input.size() - offset < size)
        return result;
    result.payload = input.mid(offset, size);
    if (masked) {
        for (qsizetype index = 0; index < size; ++index) {
            const auto mask = static_cast<quint8>(input[mask_offset + (index % 4)]);
            result.payload[index] =
                static_cast<char>(static_cast<quint8>(result.payload[index]) ^ mask);
        }
    }
    result.consumed = offset + size;
    result.complete = true;
    return result;
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
            require(!connection, "import fixture permits one connection");
            connection = server.nextPendingConnection();
            connect(connection, &QLocalSocket::disconnected, this,
                    [this, socket = connection.data()] {
                        socket->disconnect(this);
                        if (connection == socket)
                            connection = nullptr;
                        socket->deleteLater();
                    });
            connect(connection, &QLocalSocket::readyRead, this, [this] { read(); });
        });
    }

    ~Source() override {
        if (connection)
            connection->disconnect(this);
    }

    void start() {
        require(root.isValid() && server.listen(path()), "import fixture listen failed");
    }

    [[nodiscard]] QString path() const { return root.filePath("import.sock"); }

    void send(const QJsonObject& message) {
        require(connection && connection->state() == QLocalSocket::ConnectedState,
                "import fixture client disconnected");
        connection->write(frame(json(message)));
    }

    QJsonObject detection;
    QString import_id = QStringLiteral("import-1");
    std::vector<QJsonObject> before_acceptance;
    std::vector<QJsonObject> after_acceptance;
    std::vector<QJsonObject> received;

  private:
    void read() {
        if (!handshaken) {
            input.append(connection->readAll());
            const auto end = input.indexOf("\r\n\r\n");
            if (end < 0)
                return;
            const auto request = input.left(end + 4);
            const auto accept = QCryptographicHash::hash(handshake_key(request) + websocket_guid,
                                                         QCryptographicHash::Sha1)
                                    .toBase64();
            connection->write(
                QByteArrayLiteral("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                                  "Connection: Upgrade\r\nSec-WebSocket-Accept: ") +
                accept + "\r\n\r\n");
            input = input.mid(end + 4);
            handshaken = true;
        } else {
            input.append(connection->readAll());
        }
        while (!input.isEmpty()) {
            const auto decoded = decode_frame(input);
            if (!decoded.complete)
                return;
            if (decoded.opcode == 0x88) {
                if (decoded.payload.size() != 2)
                    throw std::runtime_error("invalid fixture close frame");
                QByteArray close_frame;
                close_frame.append(static_cast<char>(0x88)).append(static_cast<char>(0x02));
                close_frame.append(decoded.payload);
                connection->write(close_frame);
                connection->disconnectFromServer();
                input.clear();
                return;
            }
            input.remove(0, decoded.consumed);
            const auto request = object(decoded.payload);
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
            send({{"id", id},
                  {"result", QJsonObject{{QStringLiteral("userAgent"), QStringLiteral("fixture")},
                                         {QStringLiteral("codexHome"), QStringLiteral("/fixture")},
                                         {QStringLiteral("platformFamily"), QStringLiteral("test")},
                                         {QStringLiteral("platformOs"), QStringLiteral("test")}}}});
            send({{"method", QStringLiteral("initialized")}});
        } else if (method == QLatin1String("externalAgentConfig/detect")) {
            send({{"id", id}, {"result", detection}});
        } else if (method == QLatin1String("externalAgentConfig/import")) {
            const auto parameters = request.value(QStringLiteral("params")).toObject();
            last_import = parameters;
            for (const auto& message : before_acceptance)
                send(message);
            send({{"id", id}, {"result", QJsonObject{{QStringLiteral("importId"), import_id}}}});
            for (const auto& message : after_acceptance)
                send(message);
        }
    }

  public:
    QJsonObject last_import;

  private:
    QTemporaryDir root{QStringLiteral("/tmp/lapis-importer-XXXXXX")};
    QLocalServer server;
    QPointer<QLocalSocket> connection;
    QByteArray input;
    bool handshaken{};
};

void pump(int milliseconds) {
    QEventLoop loop;
    QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
    loop.exec();
}

template <typename Predicate> bool wait_for(Predicate predicate, int milliseconds = 5000) {
    QElapsedTimer deadline;
    deadline.start();
    while (!predicate()) {
        if (deadline.elapsed() >= milliseconds)
            return false;
        pump(20);
    }
    return true;
}

QJsonObject session_item() {
    return QJsonObject{{QStringLiteral("itemType"), QStringLiteral("SESSIONS")},
                       {QStringLiteral("cwd"), QJsonValue::Null},
                       {QStringLiteral("description"), QStringLiteral("Claude sessions in HOME")},
                       {QStringLiteral("details"),
                        QJsonObject{
                            {QStringLiteral("plugins"), QJsonArray{}},
                            {QStringLiteral("skills"), QJsonArray{}},
                            {QStringLiteral("sessions"),
                             QJsonArray{QJsonObject{
                                 {QStringLiteral("cwd"), QStringLiteral("/fixture")},
                                 {QStringLiteral("path"), QStringLiteral("/fixture/session.jsonl")},
                                 {QStringLiteral("title"), QStringLiteral("fixture")}}}},
                            {QStringLiteral("mcpServers"), QJsonArray{}},
                            {QStringLiteral("hooks"), QJsonArray{}},
                            {QStringLiteral("subagents"), QJsonArray{}},
                            {QStringLiteral("commands"), QJsonArray{}},
                            {QStringLiteral("memory"), QJsonArray{}},
                        }}};
}

QJsonObject config_item() {
    return QJsonObject{{QStringLiteral("itemType"), QStringLiteral("CONFIG")},
                       {QStringLiteral("cwd"), QJsonValue::Null},
                       {QStringLiteral("description"), QStringLiteral("Claude settings")}};
}

QJsonObject completion(const QString& identifier) {
    return QJsonObject{
        {QStringLiteral("method"), QStringLiteral("externalAgentConfig/import/completed")},
        {QStringLiteral("params"),
         QJsonObject{{QStringLiteral("importId"), identifier},
                     {QStringLiteral("itemTypeResults"),
                      QJsonArray{QJsonObject{
                          {QStringLiteral("itemType"), QStringLiteral("SESSIONS")},
                          {QStringLiteral("successes"),
                           QJsonArray{QJsonObject{
                               {QStringLiteral("itemType"), QStringLiteral("SESSIONS")},
                               {QStringLiteral("cwd"), QStringLiteral("/fixture")},
                               {QStringLiteral("source"), QStringLiteral("/fixture/session.jsonl")},
                               {QStringLiteral("target"), QStringLiteral("thread-1")},
                               {QStringLiteral("title"), QStringLiteral("fixture")}}}},
                          {QStringLiteral("failures"), QJsonArray{}}}}}}}};
}

void detect_select_import_and_parse_completion() {
    Source source;
    source.detection =
        QJsonObject{{QStringLiteral("items"), QJsonArray{config_item(), session_item()}}};
    source.after_acceptance = {
        QJsonObject{
            {QStringLiteral("method"), QStringLiteral("externalAgentConfig/import/progress")},
            {QStringLiteral("params"),
             QJsonObject{{QStringLiteral("importId"), source.import_id},
                         {QStringLiteral("itemTypeResults"), QJsonArray{}}}}},
        completion(source.import_id)};
    source.start();
    Importer importer;
    bool connected = false;
    QObject::connect(&importer, &Importer::connected, [&] { connected = true; });
    importer.start(source.path());
    require(wait_for([&] { return connected && importer.phase() == Importer::Phase::ready; }),
            "importer initializes");
    require(std::any_of(source.received.begin(), source.received.end(),
                        [](const QJsonObject& message) {
                            return message.value(QStringLiteral("method")) ==
                                   QStringLiteral("initialized");
                        }),
            "importer completes the app-server handshake");
    require(importer.detect() && wait_for([&] {
                return importer.phase() == Importer::Phase::ready &&
                       !importer.detection().items.empty();
            }),
            "bounded detection completes");
    require(importer.detection().items.size() == 2 &&
                importer.detection().items[1].sessions.size() == 1 &&
                importer.detection().items[1].sessions[0].path ==
                    QStringLiteral("/fixture/session.jsonl"),
            "session detection is typed");
    require(importer.detection().items.first().payload.isEmpty(),
            "non-session detection payloads are not retained");
    require(!importer.importSessions({0, 1}) && !importer.importSessions({0}) &&
                importer.phase() == Importer::Phase::ready,
            "non-session migrations are not selectable in the first slice");
    const auto before = source.received.size();
    require(importer.importSessions({1}), "selected session import is accepted locally");
    require(wait_for([&] {
                return std::any_of(source.received.begin(), source.received.end(),
                                   [](const auto& message) {
                                       return message.value("method") ==
                                              QStringLiteral("externalAgentConfig/import");
                                   });
            }),
            "selected import reaches the source");
    const auto parameters = source.last_import;
    const auto items = parameters.value("migrationItems").toArray();
    require(items.size() == 1 && items.first().toObject() == session_item() &&
                parameters.value("migrationSource") == QStringLiteral("claude") &&
                parameters.value("source") == QStringLiteral("lapis") &&
                parameters.value("providerId") == QStringLiteral("lapis"),
            "selected payload is preserved and attribution is bounded");
    require(wait_for([&] {
                return importer.phase() == Importer::Phase::completed ||
                       importer.phase() == Importer::Phase::failed;
            }) &&
                importer.phase() == Importer::Phase::completed,
            "ID-matched progress and completion finish the task");
    require(importer.importId() == source.import_id &&
                importer.completion().import_id == source.import_id &&
                importer.progress().size() == 1 && importer.importedSessions().size() == 1 &&
                importer.importedSessions().first().target == QStringLiteral("thread-1"),
            "import identity and session targets are retained");
    pump(50);
    require(source.received.size() == before + 1,
            "terminal completion sends no retry or follow-up RPC");
}

void wrong_import_identity_is_ignored() {
    Source source;
    source.detection = QJsonObject{{QStringLiteral("items"), QJsonArray{session_item()}}};
    source.after_acceptance = {completion(QStringLiteral("other-client"))};
    source.start();
    Importer importer;
    importer.start(source.path());
    require(wait_for([&] { return importer.phase() == Importer::Phase::ready; }),
            "foreign identity fixture initializes");
    require(importer.detect() &&
                wait_for([&] { return importer.phase() == Importer::Phase::ready; }),
            "foreign identity fixture detects");
    require(importer.importSessions({0}) &&
                wait_for([&] { return importer.phase() == Importer::Phase::importing; }),
            "foreign identity fixture accepts the import");
    pump(80);
    require(importer.phase() == Importer::Phase::importing &&
                importer.completion().import_id.isEmpty(),
            "another import's completion does not resolve this task");
}

void completion_before_acceptance_is_replayed() {
    Source source;
    source.detection = QJsonObject{{QStringLiteral("items"), QJsonArray{session_item()}}};
    source.before_acceptance = {completion(source.import_id)};
    source.start();
    Importer importer;
    importer.start(source.path());
    require(wait_for([&] { return importer.phase() == Importer::Phase::ready; }),
            "early-completion fixture initializes");
    require(importer.detect() &&
                wait_for([&] { return importer.phase() == Importer::Phase::ready; }),
            "early-completion fixture detects");
    require(importer.importSessions({0}) &&
                wait_for([&] { return importer.phase() == Importer::Phase::completed; }),
            "a completion racing the accept reply still finishes the task");
    require(importer.importedSessions().size() == 1 &&
                importer.completion().import_id == source.import_id,
            "early completion retains its identity");
}

void non_session_migration_details_fail_closed() {
    Source source;
    auto item = session_item();
    auto details = item.value(QStringLiteral("details")).toObject();
    details.insert(QStringLiteral("skills"),
                   QJsonArray{QJsonObject{{QStringLiteral("name"), QStringLiteral("other")}}});
    item.insert(QStringLiteral("details"), details);
    source.detection = QJsonObject{{QStringLiteral("items"), QJsonArray{item}}};
    source.start();
    Importer importer;
    QString failure;
    QObject::connect(&importer, &Importer::failed,
                     [&](const QString& reason) { failure = reason; });
    importer.start(source.path());
    require(wait_for([&] { return importer.phase() == Importer::Phase::ready; }),
            "mixed-class fixture initializes");
    require(importer.detect() &&
                wait_for([&] { return importer.phase() == Importer::Phase::failed; }) &&
                failure.contains(QStringLiteral("non-session migration class")),
            "a session item cannot carry another migration class");
}

void incomplete_session_details_fail_closed() {
    Source source;
    auto item = session_item();
    auto details = item.value(QStringLiteral("details")).toObject();
    details.remove(QStringLiteral("plugins"));
    item.insert(QStringLiteral("details"), details);
    source.detection = QJsonObject{{QStringLiteral("items"), QJsonArray{item}}};
    source.start();
    Importer importer;
    QString failure;
    QObject::connect(&importer, &Importer::failed,
                     [&](const QString& reason) { failure = reason; });
    importer.start(source.path());
    require(wait_for([&] { return importer.phase() == Importer::Phase::ready; }),
            "incomplete-details fixture initializes");
    require(importer.detect() &&
                wait_for([&] { return importer.phase() == Importer::Phase::failed; }) &&
                failure.contains(QStringLiteral("missing migration details: plugins")),
            "a session item must contain the complete expanded details shape");
}

void memory_absent_session_details_remain_compatible() {
    Source source;
    auto item = session_item();
    auto details = item.value(QStringLiteral("details")).toObject();
    details.remove(QStringLiteral("memory"));
    item.insert(QStringLiteral("details"), details);
    source.detection = QJsonObject{{QStringLiteral("items"), QJsonArray{item}}};
    source.start();
    Importer importer;
    QString failure;
    QObject::connect(&importer, &Importer::failed,
                     [&](const QString& reason) { failure = reason; });
    importer.start(source.path());
    require(wait_for([&] { return importer.phase() == Importer::Phase::ready; }),
            "memory-absent fixture initializes");
    require(
        importer.detect() && wait_for([&] { return importer.phase() == Importer::Phase::ready; }) &&
            importer.detection().items.size() == 1 &&
            importer.detection().items.first().sessions.size() == 1 &&
            importer.detection().items.first().sessions.first().path ==
                QStringLiteral("/fixture/session.jsonl") &&
            importer.detection().items.first().sessions.first().cwd == QStringLiteral("/fixture") &&
            failure.isEmpty(),
        "the observed pre-memory expanded shape remains compatible");
}

void nonempty_memory_session_details_fail_closed() {
    Source source;
    auto item = session_item();
    auto details = item.value(QStringLiteral("details")).toObject();
    details.insert(QStringLiteral("memory"), QJsonArray{QStringLiteral("memory")});
    item.insert(QStringLiteral("details"), details);
    source.detection = QJsonObject{{QStringLiteral("items"), QJsonArray{item}}};
    source.start();
    Importer importer;
    QString failure;
    QObject::connect(&importer, &Importer::failed,
                     [&](const QString& reason) { failure = reason; });
    importer.start(source.path());
    require(wait_for([&] { return importer.phase() == Importer::Phase::ready; }),
            "nonempty-memory fixture initializes");
    require(importer.detect() &&
                wait_for([&] { return importer.phase() == Importer::Phase::failed; }) &&
                failure.contains(QStringLiteral("non-session migration class")),
            "a nonempty memory list must not enter a session selection");
}

void malformed_shape_fails_closed_without_retry() {
    Source source;
    source.detection = QJsonObject{
        {QStringLiteral("items"),
         QJsonArray{QJsonObject{{QStringLiteral("itemType"), QStringLiteral("NEW_AGENT")},
                                {QStringLiteral("description"), QStringLiteral("new")}}}}};
    source.start();
    Importer importer;
    QString failure;
    QObject::connect(&importer, &Importer::failed,
                     [&](const QString& reason) { failure = reason; });
    importer.start(source.path());
    require(wait_for([&] { return importer.phase() == Importer::Phase::ready; }),
            "malformed fixture initializes");
    require(importer.detect() &&
                wait_for([&] { return importer.phase() == Importer::Phase::failed; }) &&
                failure.contains(QStringLiteral("unknown migration item type")),
            "unknown detection shape is terminal");
    const auto received = source.received.size();
    pump(60);
    require(source.received.size() == received && !importer.detect(),
            "failed detection never automatically retries");
}

void malformed_completion_fails_closed() {
    Source source;
    source.detection = QJsonObject{{QStringLiteral("items"), QJsonArray{session_item()}}};
    auto bad = completion(source.import_id);
    auto parameters = bad.value("params").toObject();
    parameters.remove("importId");
    bad.insert("params", parameters);
    source.after_acceptance = {bad};
    source.start();
    Importer importer;
    importer.start(source.path());
    require(wait_for([&] { return importer.phase() == Importer::Phase::ready; }),
            "completion fixture initializes");
    require(importer.detect() &&
                wait_for([&] { return importer.phase() == Importer::Phase::ready; }),
            "completion fixture detects");
    require(importer.importSessions({0}) &&
                wait_for([&] { return importer.phase() == Importer::Phase::failed; }),
            "malformed import ID fails before interpreting results");
    const auto received = source.received.size();
    pump(60);
    require(source.received.size() == received && importer.importSessions({0}) == false,
            "failed import never automatically resubmits");
}
} // namespace

int run(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    detect_select_import_and_parse_completion();
    wrong_import_identity_is_ignored();
    completion_before_acceptance_is_replayed();
    non_session_migration_details_fail_closed();
    incomplete_session_details_fail_closed();
    memory_absent_session_details_remain_compatible();
    nonempty_memory_session_details_fail_closed();
    malformed_shape_fails_closed_without_retry();
    malformed_completion_fails_closed();
    return 0;
}

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "codex-importer: " << error.what() << '\n';
        return 1;
    }
}
