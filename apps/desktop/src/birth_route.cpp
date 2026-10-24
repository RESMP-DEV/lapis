#include "birth_route.hpp"

#include "app_paths.hpp"

#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QTimer>

#include <stdexcept>
#include <utility>

namespace lapis::desktop {
namespace {

[[noreturn]] void failed(const char* message) { throw std::runtime_error(message); }

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

[[nodiscard]] QByteArray identity_bytes(const QJsonObject& root, const QString& name) {
    const auto value = root.value(name);
    if (!value.isString())
        failed("Supervisor birth reply identity is invalid");
    const auto encoded = value.toString();
    static const QRegularExpression hex_pattern{QStringLiteral("^[0-9a-f]{32}$"),
                                                QRegularExpression::CaseInsensitiveOption};
    if (!hex_pattern.match(encoded).hasMatch())
        failed("Supervisor birth reply identity is invalid");
    return QByteArray::fromHex(encoded.toLatin1());
}

[[nodiscard]] QString agent_mode(session::AgentMode mode) {
    switch (mode) {
    case session::AgentMode::codex:
        return QStringLiteral("codex");
    case session::AgentMode::claude:
        return QStringLiteral("claude");
    case session::AgentMode::terminal:
        return QStringLiteral("terminal");
    }
    failed("Unknown agent mode");
}

[[nodiscard]] QByteArray birth_frame(const BirthRequest& request) {
    QJsonArray arguments;
    for (const auto& argument : request.launch.arguments)
        arguments.append(argument);
    const QJsonObject agent{
        {QStringLiteral("mode"), agent_mode(request.launch.agent)},
        {QStringLiteral("program"), request.launch.program},
        {QStringLiteral("arguments"), arguments},
        {QStringLiteral("working_directory"), request.launch.directory},
        {QStringLiteral("columns"), static_cast<int>(request.launch.size.columns)},
        {QStringLiteral("rows"), static_cast<int>(request.launch.size.rows)},
    };
    const QJsonObject root{
        {QStringLiteral("version"), 1},
        {QStringLiteral("action"), QStringLiteral("birth")},
        {QStringLiteral("endpoint"), request.endpoint},
        {QStringLiteral("fingerprint"), request.fingerprint},
        {QStringLiteral("agent"), agent},
        {QStringLiteral("service"), QJsonObject{{QStringLiteral("environment"), QJsonObject{}}}},
    };
    const auto payload = QJsonDocument{root}.toJson(QJsonDocument::Compact);
    const quint32 length = static_cast<quint32>(payload.size());
    QByteArray frame;
    frame.reserve(payload.size() + 4);
    frame.append(static_cast<char>((length >> 24U) & 0xffU));
    frame.append(static_cast<char>((length >> 16U) & 0xffU));
    frame.append(static_cast<char>((length >> 8U) & 0xffU));
    frame.append(static_cast<char>(length & 0xffU));
    frame.append(payload);
    return frame;
}

[[nodiscard]] QJsonObject receive_reply(QLocalSocket& socket, int timeout_ms) {
    QByteArray buffer;
    QEventLoop loop;
    bool timed_out = false;
    const auto ready = [&] {
        buffer += socket.readAll();
        if (buffer.size() < 4)
            return;
        const quint32 length = static_cast<quint32>(static_cast<unsigned char>(buffer[0])) << 24U |
                               static_cast<quint32>(static_cast<unsigned char>(buffer[1])) << 16U |
                               static_cast<quint32>(static_cast<unsigned char>(buffer[2])) << 8U |
                               static_cast<quint32>(static_cast<unsigned char>(buffer[3]));
        if (length == 0 || length > 1024 * 1024)
            failed("Supervisor birth reply length is invalid");
        if (buffer.size() >= 4 + static_cast<int>(length))
            loop.quit();
    };
    QObject::connect(&socket, &QLocalSocket::readyRead, &loop, ready);
    QObject::connect(&socket, &QLocalSocket::disconnected, &loop, &QEventLoop::quit);
    QTimer::singleShot(timeout_ms, &loop, [&] {
        timed_out = true;
        loop.quit();
    });
    ready();
    loop.exec();
    if (timed_out || buffer.size() < 4)
        failed("Supervisor birth reply timed out");
    const quint32 length = static_cast<quint32>(static_cast<unsigned char>(buffer[0])) << 24U |
                           static_cast<quint32>(static_cast<unsigned char>(buffer[1])) << 16U |
                           static_cast<quint32>(static_cast<unsigned char>(buffer[2])) << 8U |
                           static_cast<quint32>(static_cast<unsigned char>(buffer[3]));
    if (buffer.size() != 4 + static_cast<int>(length))
        failed("Supervisor birth reply was disconnected");
    const auto document = QJsonDocument::fromJson(buffer.mid(4, static_cast<int>(length)));
    if (!document.isObject())
        failed("Supervisor birth reply is not an object");
    return document.object();
}
} // namespace

DirectBirthRoute::DirectBirthRoute(ServiceLauncher launcher) : launcher_{std::move(launcher)} {}

BirthTicket DirectBirthRoute::birth(const BirthRequest& request) {
    BirthTicket ticket;
    ticket.session_id = session::wire::new_id();
    const QStringList arguments = QStringList{QStringLiteral("--session-id"),
                                              QString::fromLatin1(ticket.session_id.toHex())} +
                                  request.service_arguments;
    const ServiceLaunchRequest launch_request{session_service_program(), arguments, request.log,
                                              request.environment};
    const bool started =
        launcher_ ? launcher_(launch_request) : start_service_detached(launch_request);
    if (!started)
        failed("Could not start session service");
    return ticket;
}

SupervisorBirthRoute::SupervisorBirthRoute(QString control_endpoint, int timeout_ms)
    : control_endpoint_{std::move(control_endpoint)}, timeout_ms_{timeout_ms} {}

BirthTicket SupervisorBirthRoute::birth(const BirthRequest& request) {
    if (!request.environment.isEmpty())
        failed("Supervisor birth refuses service environment overrides");

    QLocalSocket socket;
    socket.connectToServer(control_endpoint_);
    if (!socket.waitForConnected(timeout_ms_))
        failed("Could not connect the supervisor birth route");
    const auto frame = birth_frame(request);
    if (socket.write(frame) != frame.size())
        failed("Could not send the supervisor birth request");

    const auto reply = receive_reply(socket, timeout_ms_);
    if (reply.value(QStringLiteral("status")).toString() == QLatin1String("rejected"))
        failed("Supervisor refused the session birth");
    const bool exact_reply =
        reply.contains(QStringLiteral("version")) && reply.contains(QStringLiteral("status")) &&
        reply.contains(QStringLiteral("session_id")) && reply.contains(QStringLiteral("epoch")) &&
        reply.size() == 4 && reply.value(QStringLiteral("version")).toInt() == 1 &&
        reply.value(QStringLiteral("status")).toString() == QLatin1String("born");
    if (!exact_reply)
        failed("Supervisor birth reply is invalid");
    return {identity_bytes(reply, QStringLiteral("session_id")),
            identity_bytes(reply, QStringLiteral("epoch"))};
}

} // namespace lapis::desktop
