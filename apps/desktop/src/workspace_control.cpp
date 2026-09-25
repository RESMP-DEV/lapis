#include "workspace_control.hpp"

#include "platform/posix/local_endpoint.hpp"
#include "workspace.hpp"
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QTimer>
#include <exception>
#include <memory>

namespace lapis::desktop {
namespace {
constexpr qsizetype max_request = qsizetype{16} * 1024;
constexpr int idle_ms = 5000;

QByteArray reply(const QJsonObject& object) {
    return QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
}
QByteArray refusal(const QString& message) {
    return reply({{QStringLiteral("ok"), false}, {QStringLiteral("error"), message}});
}
} // namespace

QString WorkspaceControl::path(const QString& registry) {
    return QDir(QFileInfo(registry).absolutePath())
        .filePath(QStringLiteral("workspace-control.sock"));
}

WorkspaceControl::WorkspaceControl(Workspace& workspace, bool host, QObject* parent)
    : QObject(parent), workspace_(workspace), host_(host) {
    // Only the process holding the registry lock gets here, so a socket left
    // at this path is from one that died.
    try {
        const auto endpoint = session::posix::prepare_endpoint(path(workspace_.storagePath()));
        QLocalServer::removeServer(endpoint);
        server_.setSocketOptions(QLocalServer::UserAccessOption);
        if (!server_.listen(endpoint))
            qWarning().noquote() << "Workspace requests unavailable:" << server_.errorString();
    } catch (const std::exception& error) {
        qWarning().noquote() << "Workspace requests unavailable:" << error.what();
    }
    connect(&server_, &QLocalServer::newConnection, this, &WorkspaceControl::accept);
}

WorkspaceControl::~WorkspaceControl() { server_.close(); }

void WorkspaceControl::accept() {
    while (auto* socket = server_.nextPendingConnection()) {
        connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
        auto* idle = new QTimer(socket);
        idle->setSingleShot(true);
        connect(idle, &QTimer::timeout, socket, &QLocalSocket::abort);
        idle->start(idle_ms);
        auto buffer = std::make_shared<QByteArray>();
        connect(socket, &QLocalSocket::readyRead, this, [this, socket, buffer] {
            *buffer += socket->readAll();
            const auto end = buffer->indexOf('\n');
            if (end < 0 && buffer->size() <= max_request)
                return;
            // One request per connection; anything after it is ignored.
            disconnect(socket, &QLocalSocket::readyRead, this, nullptr);
            socket->write(end < 0 || end > max_request
                              ? refusal(QStringLiteral("Request too large"))
                              : answer(buffer->left(end)));
            socket->disconnectFromServer();
        });
    }
}

QByteArray WorkspaceControl::answer(const QByteArray& line) {
    QJsonParseError parse{};
    const auto document = QJsonDocument::fromJson(line, &parse);
    if (parse.error != QJsonParseError::NoError || !document.isObject())
        return refusal(QStringLiteral("Malformed request"));
    const auto request = document.object();
    if (request.value(QStringLiteral("version")).toInt() != 1)
        return refusal(QStringLiteral("Unsupported request version"));
    const auto kind = request.value(QStringLiteral("request")).toString();
    if (kind == QStringLiteral("harnesses"))
        return reply({{QStringLiteral("ok"), true},
                      {QStringLiteral("harnesses"),
                       QJsonArray::fromVariantList(workspace_.availableHarnesses())},
                      {QStringLiteral("defaults"),
                       QJsonObject::fromVariantMap(workspace_.agentDefaults())}});
    if (kind == QStringLiteral("createAgent"))
        return create(request);
    if (kind == QStringLiteral("createCategory")) {
        const auto name = request.value(QStringLiteral("name"));
        if (!name.isString())
            return refusal(QStringLiteral("Missing name"));
        const auto id = workspace_.createCategory(name.toString());
        return id.isEmpty() ? refusal(workspace_.workspaceError())
                            : reply({{QStringLiteral("ok"), true}, {QStringLiteral("id"), id}});
    }
    // Ends the agent, as Command-W on the Mac does after it asks.
    if (kind == QStringLiteral("closeAgent")) {
        const auto id = request.value(QStringLiteral("id")).toString();
        if (workspace_.session(id) == nullptr)
            return refusal(QStringLiteral("No such agent"));
        return workspace_.closeSession(id, false) ? reply({{QStringLiteral("ok"), true}})
                                                  : refusal(workspace_.workspaceError());
    }
    if (kind == QStringLiteral("handover")) {
        if (!host_)
            return refusal(QStringLiteral("A lapis window keeps this workspace"));
        // After the reply is written.
        QTimer::singleShot(0, this, &WorkspaceControl::handoverRequested);
        return reply({{QStringLiteral("ok"), true}});
    }
    return refusal(QStringLiteral("Unknown request"));
}

QByteArray WorkspaceControl::create(const QJsonObject& request) {
    for (const auto* field : {"category", "harness", "directory"})
        if (!request.value(QLatin1String(field)).isString())
            return refusal(QStringLiteral("Missing %1").arg(QLatin1String(field)));
    const auto directory = request.value(QStringLiteral("directory")).toString().trimmed();
    auto title = request.value(QStringLiteral("title")).toString().trimmed();
    if (title.isEmpty()) {
        // As the desktop form does: the project folder's name.
        const auto parts = QDir::cleanPath(directory).split(QLatin1Char('/'));
        title = parts.constLast().isEmpty() ? QStringLiteral("/") : parts.constLast();
    }
    for (const auto* field : {"machine", "program", "title", "model", "mode"})
        if (request.contains(QLatin1String(field)) &&
            !request.value(QLatin1String(field)).isString())
            return refusal(QStringLiteral("Invalid %1").arg(QLatin1String(field)));
    const auto id =
        workspace_.startAgent({.category = request.value(QStringLiteral("category")).toString(),
                               .directory = directory,
                               .title = title.left(80),
                               .harness = request.value(QStringLiteral("harness")).toString(),
                               .machine = request.value(QStringLiteral("machine")).toString(),
                               .program = request.value(QStringLiteral("program")).toString(),
                               .model = request.value(QStringLiteral("model")).toString(),
                               .mode = request.value(QStringLiteral("mode")).toString(),
                               .select = false});
    if (id.isEmpty())
        return refusal(workspace_.workspaceError());
    const auto* item = workspace_.session(id);
    return reply({{QStringLiteral("ok"), true},
                  {QStringLiteral("id"), id},
                  {QStringLiteral("updating"), item != nullptr && item->updating()}});
}

bool WorkspaceControl::requestHandover(const QString& registry) {
    QLocalSocket socket;
    socket.connectToServer(path(registry));
    if (!socket.waitForConnected(500))
        return false;
    socket.write(R"({"version":1,"request":"handover"})"
                 "\n");
    if (!socket.waitForBytesWritten(500))
        return false;
    QByteArray answer;
    while (!answer.contains('\n') && answer.size() < 4096 && socket.waitForReadyRead(1000))
        answer += socket.readAll();
    return QJsonDocument::fromJson(answer.trimmed()).object().value(QStringLiteral("ok")).toBool();
}
} // namespace lapis::desktop
