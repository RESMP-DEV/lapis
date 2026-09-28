#include "terminals.hpp"

#include "platform/posix/local_endpoint.hpp"
#include "workspace.hpp"
#include <QColor>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTimer>
#include <QUuid>
#include <algorithm>
#include <array>
#include <stdexcept>

namespace lapis::desktop {
namespace {
constexpr int kIncludeDepth = 8;
constexpr std::array kRecoveryDelays{100, 200, 400};
constexpr int kRecoveryAttempts = static_cast<int>(kRecoveryDelays.size());

void read_ssh_config(const QString& path, QStringList& hosts, int depth);

// A Host line's names, without patterns.
void add_hosts(const QStringList& names, QStringList& hosts) {
    static const QRegularExpression pattern(QStringLiteral(R"([*?!])"));
    for (const auto& name : names)
        if (!name.contains(pattern) && !hosts.contains(name))
            hosts.append(name);
}

// An Include line's files: relative to ~/.ssh, globs allowed.
void include(const QStringList& patterns, QStringList& hosts, int depth) {
    const QDir ssh(QDir::home().filePath(QStringLiteral(".ssh")));
    for (auto pattern : patterns) {
        if (pattern.startsWith(QStringLiteral("~/")))
            pattern = QDir::homePath() + pattern.mid(1);
        else if (!QDir::isAbsolutePath(pattern))
            pattern = ssh.filePath(pattern);
        const QFileInfo where(pattern);
        for (const auto& match :
             QDir(where.absolutePath()).entryInfoList({where.fileName()}, QDir::Files, QDir::Name))
            read_ssh_config(match.absoluteFilePath(), hosts, depth + 1);
    }
}

void read_ssh_config(const QString& path, QStringList& hosts, int depth) {
    QFile file(path);
    if (depth > kIncludeDepth || !file.open(QIODevice::ReadOnly | QIODevice::Text))
        return;
    static const QRegularExpression separator(QStringLiteral(R"([\s=]+)"));
    while (!file.atEnd()) {
        const auto line = QString::fromUtf8(file.readLine()).trimmed();
        if (line.startsWith(QLatin1Char('#')))
            continue;
        const auto words = line.split(separator, Qt::SkipEmptyParts);
        if (words.size() < 2)
            continue;
        const auto keyword = words.front().toLower();
        if (keyword == QLatin1String("host"))
            add_hosts(words.mid(1), hosts);
        else if (keyword == QLatin1String("include"))
            include(words.mid(1), hosts, depth);
    }
}

bool answers(const QString& endpoint) {
    if (!QFileInfo::exists(endpoint))
        return false;
    QLocalSocket probe;
    probe.connectToServer(endpoint);
    if (probe.waitForConnected(250)) {
        probe.abort();
        return true;
    }
    // A socket file alone does not prove that its service can answer.
    return false;
}

QString shell_program() {
    const auto shell = qEnvironmentVariable("SHELL");
    return shell.isEmpty() ? session::login_shell() : shell;
}

bool ready(const SessionPreview& session) {
    return session.connectionState() == QLatin1String("ready") && session.inputReady();
}

session::LaunchSpec saved_launch(const QString& program, const QStringList& arguments,
                                 const QString& directory) {
    // Reattachment verifies the exact saved launch against the service's
    // descriptor; it does not relaunch. Those paths may therefore have moved or
    // disappeared after the service started, unlike the inputs to a new spawn.
    const auto check = [](const QString& value) {
        if (value.isEmpty() || value.size() > 4096 || value.contains(QChar::Null))
            throw std::invalid_argument("saved launch has an invalid path");
    };
    check(program);
    check(directory);
    if (!QFileInfo(program).isAbsolute() || !QFileInfo(directory).isAbsolute())
        throw std::invalid_argument("saved launch paths must be absolute");
    qsizetype bytes = 0;
    for (const auto& argument : arguments) {
        if (argument.contains(QChar::Null))
            throw std::invalid_argument("saved launch has an invalid argument");
        bytes += argument.toUtf8().size();
    }
    if (arguments.size() > 256 || bytes > qsizetype{64} * 1024)
        throw std::invalid_argument("saved launch exceeds its size limits");
    return {.program = program,
            .arguments = arguments,
            .directory = directory,
            .size = {100, 30},
            .agent = session::AgentMode::terminal};
}
} // namespace

QStringList ssh_config_hosts(const QString& path) {
    QStringList hosts;
    read_ssh_config(path, hosts, 0);
    return hosts;
}

Terminals::Terminals(QString runtime, QString ssh_config, QObject* parent)
    : QObject(parent), runtime_(std::move(runtime)), ssh_config_(std::move(ssh_config)),
      shell_(shell_program()) {
    ssh_hosts_ = ssh_config_hosts(ssh_config_);
    // Endpoints are recorded in the canonical folder, as prepare_endpoint makes them.
    if (const auto canonical = QFileInfo(runtime_).canonicalFilePath(); !canonical.isEmpty())
        runtime_ = canonical;
}

Terminals::~Terminals() = default;

QString Terminals::registryPath() const {
    return QDir(runtime_).filePath(QStringLiteral("terminals.json"));
}

QVariantList Terminals::machines() const {
    QVariantList list;
    const auto add = [&](const QString& id, const QString& name) {
        const bool open = std::any_of(entries_.cbegin(), entries_.cend(), [&](const Entry& entry) {
            return entry.machine == id && entry.session && !closed(entry);
        });
        list.append(QVariantMap{{QStringLiteral("id"), id},
                                {QStringLiteral("name"), name},
                                {QStringLiteral("open"), open}});
    };
    add({}, QStringLiteral("This Mac"));
    for (const auto& host : ssh_hosts_)
        add(host, host);
    return list;
}

SessionPreview* Terminals::current() const {
    for (const auto& entry : entries_)
        if (entry.machine == current_machine_)
            return entry.session.get();
    return nullptr;
}

SessionPreview* Terminals::terminal(const QString& id) const {
    for (const auto& entry : entries_)
        if (entry.id == id)
            return entry.session.get();
    return nullptr;
}

bool Terminals::fail(const QString& message) {
    error_ = message;
    emit errorChanged();
    return false;
}

bool Terminals::closed(const Entry& entry) const {
    if (entry.session == nullptr)
        return true;
    const auto& state = entry.session->connectionState();
    if (state == QLatin1String("ended") || state == QLatin1String("replaced"))
        return true;
    // An accepted close owns the entry until it can actually terminate it.
    if (entry.close_pending)
        return false;
    if (state != QLatin1String("disconnected"))
        return false;
    if (!entry.attach_started)
        return false;
    return !entry.attach_synchronized;
}

session::LaunchSpec Terminals::launchFor(const QString& machine) const {
    // This Mac: the login shell at home. A host: ssh gives its own login shell,
    // over its own connection: one shared through the user's ControlMaster
    // ends with the ssh that opened it, taking every other session with it.
    if (machine.isEmpty())
        return session::validate_launch({.program = shell_,
                                         .arguments = {QStringLiteral("-l"), QStringLiteral("-i")},
                                         .directory = QDir::homePath()});
    return session::validate_launch(
        {.program = QStandardPaths::findExecutable(QStringLiteral("ssh")),
         .arguments = {QStringLiteral("-o"), QStringLiteral("ControlPath=none"),
                       QStringLiteral("-t"), QStringLiteral("--"), machine},
         .directory = QDir::homePath()});
}

Terminals::Entry* Terminals::find(const QString& machine) {
    for (auto& entry : entries_)
        if (entry.machine == machine)
            return &entry;
    return nullptr;
}

Terminals::Entry* Terminals::entryFor(const QString& id) {
    const auto found = std::find_if(entries_.begin(), entries_.end(),
                                    [&](const Entry& entry) { return entry.id == id; });
    return found != entries_.end() ? &*found : nullptr;
}

void Terminals::attach(Entry& entry, bool create) {
    const auto title = entry.machine.isEmpty() ? QStringLiteral("This Mac") : entry.machine;
    entry.session = std::make_unique<SessionPreview>(title, entry.launch.directory, QString{},
                                                     QColor(QStringLiteral("#87cbac")), "");
    entry.session->setSessionId(entry.id);
    entry.session->setHarnessId(QStringLiteral("shell"));
    entry.session->setStatusSource(SessionPreview::StatusSource::output);
    watch(entry);
    entry.session->startLive(entry.endpoint, entry.launch,
                             create ? session::wire::AttachMode::create
                                    : session::wire::AttachMode::reconnect);
}

void Terminals::watch(Entry& entry) {
    // A shell that exits leaves; the next show() starts a fresh one.
    const auto id = entry.id;
    connect(entry.session.get(), &SessionPreview::connectionChanged, this, [this, id] {
        auto* session = terminal(id);
        if (session == nullptr)
            return;
        auto* found = entryFor(id);
        if (found == nullptr)
            return;
        const auto& state = session->connectionState();
        if (state == QLatin1String("connecting") || state == QLatin1String("synchronizing") ||
            state == QLatin1String("ready")) {
            found->attach_started = true;
        }
        if (state == QLatin1String("ready")) {
            found->attach_synchronized = true;
            found->recovery_attempts = 0;
            completeClose(*found);
            return;
        }
        if (state == QLatin1String("disconnected") && !found->attach_started)
            return;
        if (state == QLatin1String("disconnected") &&
            (found->attach_synchronized || found->close_pending)) {
            // A synchronized service can lose its socket without dying. Retry
            // the attach; a close waits to queue termination after recovery.
            if (found->recovery_attempts < kRecoveryAttempts) {
                ++found->recovery_attempts;
                QTimer::singleShot(
                    kRecoveryDelays.at(static_cast<std::size_t>(found->recovery_attempts - 1)),
                    this, [this, id] { recover(id); });
            } else if (found->close_pending) {
                fail(QStringLiteral("Could not reconnect the terminal to queue termination."));
            } else {
                fail(QStringLiteral("Terminal disconnected; reopen it to retry the connection."));
            }
            return;
        }
        QMetaObject::invokeMethod(this, [this, id] { discardIfClosed(id); }, Qt::QueuedConnection);
    });
}

void Terminals::completeClose(Entry& entry) {
    if (!entry.close_pending || !ready(*entry.session))
        return;
    if (entry.session->terminate()) {
        entry.session->setClosing(true);
        if (!error_.isEmpty()) {
            error_.clear();
            emit errorChanged();
        }
        return;
    }
    entry.close_pending = false;
    entry.session->setClosing(false);
    fail(QStringLiteral("Could not queue termination; the terminal remains open."));
}

Terminals::Entry* Terminals::start(const QString& machine) {
    if (!machine.isEmpty() && !ssh_hosts_.contains(machine)) {
        fail(QStringLiteral("%1 is not a host in your ssh config.").arg(machine));
        return nullptr;
    }
    try {
        Entry entry;
        entry.id = QStringLiteral("terminal-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
        entry.machine = machine;
        entry.launch = launchFor(machine);
        entry.endpoint = session::posix::prepare_endpoint(
            QDir(runtime_).filePath(entry.id + QStringLiteral(".sock")));
        entries_.push_back(std::move(entry));
        attach(entries_.back(), true);
        save();
        emit machinesChanged();
        return &entries_.back();
    } catch (const std::exception& problem) {
        fail(QStringLiteral("Could not start a terminal: %1")
                 .arg(QString::fromUtf8(problem.what())));
        return nullptr;
    }
}

QString Terminals::open(const QString& machine) {
    auto* entry = find(machine);
    if (entry != nullptr && entry->session && closed(*entry)) {
        discard(entry->id);
        entry = nullptr;
    }
    if (entry == nullptr)
        entry = start(machine);
    else if (entry->session && entry->session->connectionState() == QLatin1String("disconnected")) {
        entry->recovery_attempts = 0;
        recover(entry->id);
    }
    return entry != nullptr ? entry->id : QString{};
}

bool Terminals::show(const QString& machine) {
    if (open(machine).isEmpty())
        return false;
    if (!error_.isEmpty()) {
        error_.clear();
        emit errorChanged();
    }
    current_machine_ = machine;
    emit currentChanged();
    return true;
}

bool Terminals::close(const QString& id) {
    auto* session = terminal(id);
    if (session == nullptr)
        return false;
    const auto found = std::find_if(entries_.cbegin(), entries_.cend(),
                                    [&](const Entry& entry) { return entry.id == id; });
    if (found != entries_.cend() && closed(*found))
        discard(id);
    else {
        auto* entry = entryFor(id);
        if (entry != nullptr && (!entry->close_pending ||
                                 (session->connectionState() == QLatin1String("disconnected") &&
                                  entry->recovery_attempts >= kRecoveryAttempts))) {
            entry->session->setClosing(true);
            entry->close_pending = true;
            if (!session->terminate()) {
                if (ready(*session)) {
                    entry->close_pending = false;
                    session->setClosing(false);
                    return fail(
                        QStringLiteral("Could not queue termination; the terminal remains open."));
                }
                fail(QStringLiteral("Terminal close will follow synchronization."));
                if (entry->attach_started &&
                    session->connectionState() == QLatin1String("disconnected")) {
                    entry->recovery_attempts = 0;
                    recover(id);
                }
            }
        }
    }
    return true;
}

void Terminals::discardIfClosed(const QString& id) {
    // A queued callback can race an explicit reconnect: re-read the entry and
    // its current connection state before removing it.
    const auto found = std::find_if(entries_.cbegin(), entries_.cend(),
                                    [&](const Entry& entry) { return entry.id == id; });
    if (found != entries_.cend() && closed(*found))
        discard(id);
}

void Terminals::recover(const QString& id) {
    auto* entry = entryFor(id);
    if (entry == nullptr || entry->session == nullptr ||
        entry->session->connectionState() != QLatin1String("disconnected"))
        return;
    if (entry->close_pending && !entry->attach_synchronized)
        entry->session->discoverSession();
    else if (entry->attach_synchronized)
        entry->session->reconnect();
}

void Terminals::discard(const QString& id) {
    const auto found = std::find_if(entries_.begin(), entries_.end(),
                                    [&](const Entry& entry) { return entry.id == id; });
    if (found == entries_.end())
        return;
    const bool shown = found->machine == current_machine_;
    // The session may be mid-signal; let its object go on the next turn.
    if (found->session)
        found->session.release()->deleteLater();
    entries_.erase(found);
    save();
    emit machinesChanged();
    if (shown)
        emit currentChanged();
}

void Terminals::restore() {
    QFile file(registryPath());
    if (!file.open(QIODevice::ReadOnly))
        return;
    const auto saved = QJsonDocument::fromJson(file.readAll()).object();
    if (saved.value(QStringLiteral("version")).toInt() != 1)
        return;
    for (const auto& value : saved.value(QStringLiteral("terminals")).toArray()) {
        const auto record = value.toObject();
        Entry entry;
        entry.id = record.value(QStringLiteral("id")).toString();
        entry.machine = record.value(QStringLiteral("machine")).toString();
        entry.endpoint = record.value(QStringLiteral("endpoint")).toString();
        // Only this folder's own endpoints, one terminal per machine.
        if (!entry.id.startsWith(QStringLiteral("terminal-")) ||
            entry.endpoint != QDir(runtime_).filePath(entry.id + QStringLiteral(".sock")) ||
            find(entry.machine) != nullptr || !answers(entry.endpoint))
            continue;
        QStringList arguments;
        for (const auto& argument : record.value(QStringLiteral("arguments")).toArray())
            arguments.append(argument.toString());
        try {
            entry.launch =
                saved_launch(record.value(QStringLiteral("program")).toString(), arguments,
                             record.value(QStringLiteral("directory")).toString());
        } catch (const std::exception& problem) {
            qWarning().noquote() << "Terminal not restored:" << problem.what();
            continue;
        }
        entries_.push_back(std::move(entry));
        attach(entries_.back(), false);
    }
    save();
    emit machinesChanged();
}

void Terminals::save() const {
    QJsonArray list;
    for (const auto& entry : entries_) {
        QJsonArray arguments;
        for (const auto& argument : entry.launch.arguments)
            arguments.append(argument);
        list.append(QJsonObject{{QStringLiteral("id"), entry.id},
                                {QStringLiteral("machine"), entry.machine},
                                {QStringLiteral("endpoint"), entry.endpoint},
                                {QStringLiteral("program"), entry.launch.program},
                                {QStringLiteral("arguments"), arguments},
                                {QStringLiteral("directory"), entry.launch.directory}});
    }
    QSaveFile out(registryPath());
    if (!out.open(QIODevice::WriteOnly))
        qWarning().noquote() << "Terminals not saved:" << registryPath() << out.errorString();
    else if (!out.setPermissions(QFile::ReadOwner | QFile::WriteOwner))
        qWarning().noquote() << "Terminals not saved privately:" << registryPath()
                             << out.errorString();
    else {
        const auto document = QJsonDocument(QJsonObject{{QStringLiteral("version"), 1},
                                                        {QStringLiteral("terminals"), list}})
                                  .toJson(QJsonDocument::Compact);
        if (out.write(document) != document.size() || !out.commit())
            qWarning().noquote() << "Terminals not saved:" << registryPath() << out.errorString();
    }
}
} // namespace lapis::desktop
