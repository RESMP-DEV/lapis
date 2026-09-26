#include "workspace_control.hpp"

#include "keymap.hpp"
#include "platform/posix/local_endpoint.hpp"
#include "terminals.hpp"
#include "workspace.hpp"
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QTimer>
#include <algorithm>
#include <array>
#include <exception>
#include <memory>
#include <string_view>
#include <utility>

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
    using Handler = QByteArray (WorkspaceControl::*)(const QString&, const QJsonObject&);
    static constexpr std::array<std::pair<std::string_view, Handler>, 15> handlers{{
        {"harnesses", &WorkspaceControl::harnesses},
        {"createAgent", &WorkspaceControl::create},
        {"createCategory", &WorkspaceControl::category},
        {"renameCategory", &WorkspaceControl::category},
        {"removeCategory", &WorkspaceControl::category},
        {"placeCategory", &WorkspaceControl::category},
        {"closeAgent", &WorkspaceControl::agent},
        {"renameAgent", &WorkspaceControl::agent},
        {"placeAgent", &WorkspaceControl::agent},
        {"restartAgent", &WorkspaceControl::agent},
        {"openTerminal", &WorkspaceControl::terminal},
        {"closeTerminal", &WorkspaceControl::terminal},
        {"settings", &WorkspaceControl::settings},
        {"changeSettings", &WorkspaceControl::settings},
        {"handover", &WorkspaceControl::handover},
    }};
    const auto kind = request.value(QStringLiteral("request")).toString();
    for (const auto& [name, handler] : handlers)
        if (kind == QLatin1String(name.data(), static_cast<qsizetype>(name.size())))
            return (this->*handler)(kind, request);
    return refusal(QStringLiteral("Unknown request"));
}

QByteArray WorkspaceControl::refused() {
    auto message = workspace_.workspaceError();
    workspace_.clearError();
    return refusal(message.isEmpty() ? QStringLiteral("The Mac refused the change") : message);
}

QByteArray WorkspaceControl::harnesses(const QString& /*kind*/, const QJsonObject& /*request*/) {
    return reply(
        {{QStringLiteral("ok"), true},
         {QStringLiteral("harnesses"),
          QJsonArray::fromVariantList(workspace_.availableHarnesses())},
         {QStringLiteral("defaults"), QJsonObject::fromVariantMap(workspace_.agentDefaults())}});
}

QByteArray WorkspaceControl::handover(const QString& /*kind*/, const QJsonObject& /*request*/) {
    if (!host_)
        return refusal(QStringLiteral("A lapis window keeps this workspace"));
    // After the reply is written.
    QTimer::singleShot(0, this, &WorkspaceControl::handoverRequested);
    return reply({{QStringLiteral("ok"), true}});
}

QByteArray WorkspaceControl::create(const QString& /*kind*/, const QJsonObject& request) {
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
    for (const auto* field : {"machine", "program", "title", "model", "mode", "resume"})
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
                               .select = false,
                               .resume = request.value(QStringLiteral("resume")).toString()});
    if (id.isEmpty())
        return refused();
    const auto* item = workspace_.session(id);
    return reply({{QStringLiteral("ok"), true},
                  {QStringLiteral("id"), id},
                  {QStringLiteral("updating"), item != nullptr && item->updating()}});
}

// Categories change as the Mac's own commands change them, under the same
// rules (a removed category must be empty, and one always stays), while the
// window keeps the category it shows.
QByteArray WorkspaceControl::category(const QString& kind, const QJsonObject& request) {
    const auto id = request.value(QStringLiteral("id")).toString();
    const auto name = request.value(QStringLiteral("name"));
    if (kind == QStringLiteral("createCategory") || kind == QStringLiteral("renameCategory")) {
        if (!name.isString())
            return refusal(QStringLiteral("Missing name"));
        if (kind == QStringLiteral("renameCategory"))
            return workspace_.renameCategory(id, name.toString())
                       ? reply({{QStringLiteral("ok"), true}})
                       : refused();
        const auto made = workspace_.createCategory(name.toString());
        return made.isEmpty() ? refused()
                              : reply({{QStringLiteral("ok"), true}, {QStringLiteral("id"), made}});
    }
    if (kind == QStringLiteral("removeCategory"))
        return workspace_.removeCategory(id) ? reply({{QStringLiteral("ok"), true}}) : refused();
    const auto index = request.value(QStringLiteral("index"));
    if (!index.isDouble())
        return refusal(QStringLiteral("Missing index"));
    return workspace_.placeCategory(id, index.toInt()) ? reply({{QStringLiteral("ok"), true}})
                                                       : refused();
}

// Ends the agent, as Command-W on the Mac does after it asks; names it as
// Rename agent does there (that name stays over its conversation's title);
// puts it at a position in a category's strip; or restarts it once stopped.
QByteArray WorkspaceControl::agent(const QString& kind, const QJsonObject& request) {
    const auto id = request.value(QStringLiteral("id")).toString();
    if (workspace_.session(id) == nullptr)
        return refusal(QStringLiteral("No such agent"));
    bool done = false;
    if (kind == QStringLiteral("renameAgent")) {
        const auto title = request.value(QStringLiteral("title"));
        if (!title.isString())
            return refusal(QStringLiteral("Missing title"));
        done = workspace_.renameSession(id, title.toString());
    } else if (kind == QStringLiteral("placeAgent")) {
        const auto category = request.value(QStringLiteral("category"));
        const auto index = request.value(QStringLiteral("index"));
        if (!category.isString() || !index.isDouble())
            return refusal(QStringLiteral("Missing category or index"));
        done = workspace_.placeSessions({id}, category.toString(), index.toInt());
    } else if (kind == QStringLiteral("restartAgent")) {
        done = workspace_.restartAgent(id);
    } else {
        done = workspace_.closeSession(id, false);
    }
    return done ? reply({{QStringLiteral("ok"), true}}) : refused();
}

// A plain shell on a machine, apart from agents; one per machine.
QByteArray WorkspaceControl::terminal(const QString& kind, const QJsonObject& request) {
    if (terminals_ == nullptr)
        return refusal(QStringLiteral("Terminals are not available"));
    if (kind == QStringLiteral("closeTerminal"))
        return terminals_->close(request.value(QStringLiteral("id")).toString())
                   ? reply({{QStringLiteral("ok"), true}})
                   : refusal(QStringLiteral("No such terminal"));
    const auto machine = request.value(QStringLiteral("machine"));
    if (!machine.isString() && !machine.isUndefined())
        return refusal(QStringLiteral("Invalid machine"));
    const auto id = terminals_->open(machine.toString());
    return id.isEmpty() ? refusal(terminals_->error())
                        : reply({{QStringLiteral("ok"), true}, {QStringLiteral("id"), id}});
}

namespace {
// The Mac's settings that matter away from it, which its Settings window
// also sets: staying awake for the phone, the Mac's alerts, and plan usage.
// How the window looks stays the Mac's to choose.
struct Switch {
    std::string_view name;
    bool (KeyMap::*get)() const;
    bool (KeyMap::*set)(bool);
};
constexpr std::array<Switch, 5> switches{{
    {"keepAwake", &KeyMap::keepAwake, &KeyMap::setKeepAwake},
    {"alertSound", &KeyMap::alertSound, &KeyMap::setAlertSound},
    {"finishSound", &KeyMap::finishSound, &KeyMap::setFinishSound},
    {"notify", &KeyMap::notify, &KeyMap::setNotify},
    {"showUsage", &KeyMap::showUsage, &KeyMap::setShowUsage},
}};
constexpr auto repeat_name = std::string_view{"alertRepeat"};

QString text(std::string_view name) {
    return QString::fromLatin1(name.data(), static_cast<qsizetype>(name.size()));
}

QJsonObject current_settings(const KeyMap& keymap) {
    QJsonObject values{{text(repeat_name), keymap.alertRepeat()}};
    for (const auto& item : switches)
        values.insert(text(item.name), (keymap.*item.get)());
    return values;
}

// Why `changes` cannot be applied, or empty. Nothing is applied unless all can be.
QString invalid_change(const QJsonObject& changes) {
    for (auto entry = changes.begin(); entry != changes.end(); ++entry) {
        const bool is_switch = std::any_of(switches.begin(), switches.end(), [&](const auto& item) {
            return entry.key() == text(item.name);
        });
        if (is_switch ? !entry.value().isBool()
                      : entry.key() != text(repeat_name) || !entry.value().isDouble())
            return QStringLiteral("Invalid setting %1").arg(entry.key());
    }
    return {};
}
} // namespace

QByteArray WorkspaceControl::settings(const QString& kind, const QJsonObject& request) {
    if (keymap_ == nullptr)
        return refusal(QStringLiteral("Settings are not available"));
    if (kind == QStringLiteral("changeSettings")) {
        const auto changes = request.value(QStringLiteral("settings")).toObject();
        if (const auto problem = invalid_change(changes); !problem.isEmpty())
            return refusal(problem);
        bool saved = true;
        for (const auto& item : switches)
            if (const auto value = changes.value(text(item.name)); value.isBool())
                saved = (keymap_->*item.set)(value.toBool()) && saved;
        if (const auto repeat = changes.value(text(repeat_name)); repeat.isDouble())
            saved = keymap_->setAlertRepeat(repeat.toInt()) && saved;
        if (!saved)
            return refusal(keymap_->diagnostic());
    }
    return reply(
        {{QStringLiteral("ok"), true}, {QStringLiteral("settings"), current_settings(*keymap_)}});
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
