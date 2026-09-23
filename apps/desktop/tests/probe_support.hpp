// Test-only helpers shared by the opt-in workspace GUI probes.  They are not
// product API and must not launch fixtures by themselves.
#ifndef LAPIS_DESKTOP_TEST_PROBE_SUPPORT_HPP
#define LAPIS_DESKTOP_TEST_PROBE_SUPPORT_HPP

#include "ui_preview.hpp"

#include <QByteArray>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLocalSocket>
#include <QMouseEvent>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QString>
#include <QThread>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <optional>
#include <source_location>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef Q_OS_MACOS
#include <libproc.h>
#include <sys/proc_info.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace lapis::desktop::test {

inline constexpr int default_timeout_ms = 30000;

inline void require(bool condition, const char* message,
                    std::source_location where = std::source_location::current()) {
    if (!condition)
        throw std::runtime_error(std::string(message) + " at line " + std::to_string(where.line()));
}

inline void require(bool condition, const std::string& message,
                    std::source_location where = std::source_location::current()) {
    require(condition, message.c_str(), where);
}

inline void pump(int milliseconds = 10) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    while (std::chrono::steady_clock::now() < deadline) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
}

template <typename Predicate>
inline void until(Predicate predicate, const char* message, int timeout = default_timeout_ms,
                  std::source_location where = std::source_location::current()) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (!predicate()) {
        if (elapsed.elapsed() >= timeout)
            require(false, message, where);
        pump(5);
    }
}

inline QQuickItem* visual(QQuickItem* parent, const QString& name) {
    if (parent->objectName() == name)
        return parent;
    for (auto* child : parent->childItems())
        if (auto* found = visual(child, name))
            return found;
    return nullptr;
}

inline QPointF actionable_center(QQuickWindow& window, QQuickItem* target) {
    require(target && target->isVisible() && target->isEnabled(), "Control is not actionable");
    const QPointF center(target->width() / 2, target->height() / 2);
    const QPointF scene = target->mapToScene(center);
    require(window.contentItem()->contains(scene), "Control is outside the window");
    return scene;
}

inline void click_item(QQuickWindow& window, QQuickItem* target) {
    const QPointF position = actionable_center(window, target);
    const QPointF global = window.mapToGlobal(position);
    QMouseEvent press(QEvent::MouseButtonPress, position, global, Qt::LeftButton, Qt::LeftButton,
                      Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease, position, global, Qt::LeftButton, Qt::NoButton,
                        Qt::NoModifier);
    QCoreApplication::sendEvent(&window, &press);
    QCoreApplication::sendEvent(&window, &release);
    pump(20);
}

inline void click(QQuickWindow& window, const QString& name) {
    auto* target = visual(window.contentItem(), name);
    require(target && target->isVisible() && target->isEnabled(),
            "Control is not actionable: " + name.toStdString());
    click_item(window, target);
}

inline QJsonValue read_json(const QString& path, const char* label) {
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), std::string("Could not open ") + label);
    const auto document = QJsonDocument::fromJson(file.readAll());
    require(document.isObject(), std::string(label) + " is not an object");
    return document.object();
}

inline bool write_json(const QString& path, const QJsonObject& report) {
    if (path.isEmpty())
        return true;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
        return false;
    const QByteArray bytes = QJsonDocument(report).toJson(QJsonDocument::Indented);
    return file.write(bytes) == bytes.size() && file.flush();
}

struct ChildProcesses {
    qint64 shell{};
    qint64 service{};
    bool operator==(const ChildProcesses&) const = default;
};

struct ProcessIdentity {
    qint64 pid{};
    QByteArray executable;
    std::uint64_t start_seconds{};
    std::uint64_t start_microseconds{};
};

inline bool same_process(const ProcessIdentity& left, const ProcessIdentity& right) {
    return left.pid == right.pid && left.executable == right.executable &&
           left.start_seconds == right.start_seconds &&
           left.start_microseconds == right.start_microseconds;
}

inline QString screen(const SessionPreview& session) {
    const auto& snapshot = session.snapshot();
    QString result;
    for (std::size_t row = 0; row < snapshot.size.rows; ++row) {
        for (std::size_t column = 0; column < snapshot.size.columns; ++column) {
            const auto grapheme = snapshot.text(row * snapshot.size.columns + column);
            result += QString::fromUcs4(grapheme.data(), static_cast<qsizetype>(grapheme.size()));
        }
        result += QLatin1Char('\n');
    }
    return result;
}

inline void send(SessionPreview& session, const QByteArray& command) {
    require(session.inputReady(), "Session input is not ready");
    session.sendText(command + '\r');
}

inline ChildProcesses process_probe(SessionPreview& session, const QByteArray& marker) {
    send(session, "PS1=; PS2=; unset PROMPT_COMMAND; stty -echo; printf '\\033[2J\\033[H" + marker +
                      ":%s:%s:END\\n' \"$$\" \"$PPID\"");
    const QRegularExpression ids(QLatin1String("^") +
                                     QRegularExpression::escape(QString::fromLatin1(marker)) +
                                     QStringLiteral(":(\\d+):(\\d+):END$"),
                                 QRegularExpression::MultilineOption);
    QRegularExpressionMatch match;
    until(
        [&] {
            match = ids.match(screen(session));
            return match.hasMatch();
        },
        "Observed shell process IDs timed out");
    return {match.captured(1).toLongLong(), match.captured(2).toLongLong()};
}

#ifdef Q_OS_MACOS

inline std::optional<ProcessIdentity> process_identity(qint64 pid) {
    if (pid <= 0)
        return std::nullopt;
    char executable[PROC_PIDPATHINFO_MAXSIZE]{};
    if (::proc_pidpath(static_cast<pid_t>(pid), executable, sizeof(executable)) <= 0)
        return std::nullopt;
    struct proc_bsdinfo info{};
    if (::proc_pidinfo(static_cast<pid_t>(pid), PROC_PIDTBSDINFO, 0, &info, sizeof(info)) !=
        static_cast<int>(sizeof(info)))
        return std::nullopt;
    return ProcessIdentity{pid, QByteArray(executable), info.pbi_start_tvsec,
                           info.pbi_start_tvusec};
}

inline bool process_matches(const ProcessIdentity& expected) {
    const auto current = process_identity(expected.pid);
    return current.has_value() && same_process(*current, expected);
}

inline bool process_gone(const ProcessIdentity& expected) { return !process_matches(expected); }

inline std::vector<ProcessIdentity> socket_owner_identities(const QString& endpoint) {
    // The service atomically renames its listener after bind. macOS retains the
    // temporary address in proc_pidfdinfo, so compare the connected peer identity
    // instead of matching that stale bound pathname against the public endpoint.
    QLocalSocket connection;
    connection.connectToServer(endpoint);
    if (!connection.waitForConnected(1000))
        return {};
    pid_t peer{};
    socklen_t size = sizeof(peer);
    if (::getsockopt(static_cast<int>(connection.socketDescriptor()), SOL_LOCAL, LOCAL_PEERPID,
                     &peer, &size) != 0 ||
        size != sizeof(peer))
        return {};
    const auto identity = process_identity(peer);
    if (!identity || !identity->executable.endsWith("/lapis_session_service") ||
        !process_matches(*identity))
        return {};
    return {*identity};
}

inline std::optional<ProcessIdentity> observed_service_identity(const QString& endpoint,
                                                                const ChildProcesses& child) {
    auto identity = process_identity(child.service);
    if (!identity || !identity->executable.endsWith("/lapis_session_service"))
        return std::nullopt;
    struct proc_bsdinfo info{};
    if (::proc_pidinfo(static_cast<pid_t>(child.shell), PROC_PIDTBSDINFO, 0, &info, sizeof(info)) !=
            static_cast<int>(sizeof(info)) ||
        info.pbi_pid != static_cast<uint32_t>(child.shell) ||
        info.pbi_ppid != static_cast<uint32_t>(child.service))
        return std::nullopt;
    const auto owners = socket_owner_identities(endpoint);
    const auto owner = std::find_if(owners.begin(), owners.end(), [&](const auto& candidate) {
        return same_process(*identity, candidate);
    });
    return owner == owners.end() ? std::nullopt : identity;
}

inline bool signal_verified(const ProcessIdentity& identity, int signal_number) {
    return process_matches(identity) &&
           ::kill(static_cast<pid_t>(identity.pid), signal_number) == 0;
}

inline void terminate_all(const std::vector<ProcessIdentity>& processes) {
    bool signaled = false;
    for (const auto& process : processes)
        signaled |= signal_verified(process, SIGTERM);
    if (!signaled)
        return;
    QElapsedTimer elapsed;
    elapsed.start();
    while (elapsed.elapsed() < 3000) {
        if (std::all_of(processes.begin(), processes.end(), process_gone))
            return;
        pump(20);
    }
    for (const auto& process : processes)
        static_cast<void>(signal_verified(process, SIGKILL));
    pump(500);
}

#else

inline std::optional<ProcessIdentity> observed_service_identity(const QString&,
                                                                const ChildProcesses&) {
    return std::nullopt;
}

inline bool process_gone(const ProcessIdentity&) { return false; }

inline void terminate_all(const std::vector<ProcessIdentity>&) {}

#endif

} // namespace lapis::desktop::test

#endif
