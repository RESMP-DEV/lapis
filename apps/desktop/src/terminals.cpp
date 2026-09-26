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
#include <QUuid>
#include <algorithm>

namespace lapis::desktop {
namespace {
constexpr int kIncludeDepth = 8;

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
    return probe.error() != QLocalSocket::ConnectionRefusedError &&
           probe.error() != QLocalSocket::ServerNotFoundError;
}

QString shell_program() {
    const auto shell = qEnvironmentVariable("SHELL");
    return shell.isEmpty() ? session::login_shell() : shell;
}

bool ended(const SessionPreview& session) {
    return session.connectionState() == QLatin1String("ended");
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
            return entry.machine == id && entry.session && !ended(*entry.session);
        });
        list.append(QVariantMap{{QStringLiteral("id"), id},
                                {QStringLiteral("name"), name},
                                {QStringLiteral("open"), open}});
    };
    add({}, QStringLiteral("This Mac"));
    for (const auto& host : ssh_config_hosts(ssh_config_))
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

session::LaunchSpec Terminals::launchFor(const QString& machine) const {
    // This Mac: the login shell at home. A host: ssh gives its own login shell.
    if (machine.isEmpty())
        return session::validate_launch({.program = shell_,
                                         .arguments = {QStringLiteral("-l"), QStringLiteral("-i")},
                                         .directory = QDir::homePath()});
    return session::validate_launch(
        {.program = QStandardPaths::findExecutable(QStringLiteral("ssh")),
         .arguments = {QStringLiteral("-t"), QStringLiteral("--"), machine},
         .directory = QDir::homePath()});
}

Terminals::Entry* Terminals::find(const QString& machine) {
    for (auto& entry : entries_)
        if (entry.machine == machine)
            return &entry;
    return nullptr;
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
        if (session == nullptr || !ended(*session))
            return;
        QMetaObject::invokeMethod(this, [this, id] { discard(id); }, Qt::QueuedConnection);
    });
}

Terminals::Entry* Terminals::start(const QString& machine) {
    if (!machine.isEmpty() && !ssh_config_hosts(ssh_config_).contains(machine)) {
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
    if (entry != nullptr && entry->session && ended(*entry->session)) {
        discard(entry->id);
        entry = nullptr;
    }
    if (entry == nullptr)
        entry = start(machine);
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
    if (ended(*session) || !session->terminate())
        discard(id);
    return true;
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
            entry.launch = session::validate_launch(
                {.program = record.value(QStringLiteral("program")).toString(),
                 .arguments = arguments,
                 .directory = record.value(QStringLiteral("directory")).toString()});
        } catch (const std::exception&) {
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
        return;
    out.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
    out.write(QJsonDocument(
                  QJsonObject{{QStringLiteral("version"), 1}, {QStringLiteral("terminals"), list}})
                  .toJson(QJsonDocument::Compact));
    if (!out.commit())
        qWarning().noquote() << "Terminals not saved:" << registryPath();
}
} // namespace lapis::desktop
