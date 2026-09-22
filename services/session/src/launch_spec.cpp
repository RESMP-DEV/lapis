#include "launch_spec.hpp"

#include "transport/local_protocol.hpp"
#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <stdexcept>
#include <vector>

#ifndef _WIN32
#include <pwd.h>
#include <unistd.h>
#endif

namespace lapis::session {
namespace {
void validate_agent(const LaunchSpec& launch) {
    switch (launch.agent) {
    case AgentMode::terminal:
        return;
    case AgentMode::claude:
        for (const auto& argument : launch.arguments) {
            if (argument == QStringLiteral("--"))
                break;
            const auto option = argument.section(QLatin1Char('='), 0, 0);
            if (option == QStringLiteral("--settings") || option == QStringLiteral("--bare") ||
                option == QStringLiteral("--safe-mode"))
                throw std::invalid_argument(
                    "Managed Claude owns --settings and requires hooks enabled");
        }
        return;
    case AgentMode::codex:
        for (const auto& argument : launch.arguments) {
            if (argument == QStringLiteral("--"))
                break;
            if (argument == QStringLiteral("--remote") ||
                argument.startsWith(QStringLiteral("--remote=")))
                throw std::invalid_argument("Managed Codex owns its remote endpoint");
        }
        return;
    }
    throw std::invalid_argument("Unknown agent integration mode");
}
} // namespace
LaunchSpec validate_launch(LaunchSpec launch) {
    validate_agent(launch);
    qsizetype bytes = 0;
    const auto check_text = [&bytes](const QString& value) {
        if (value.contains(QChar::Null))
            throw std::invalid_argument("Launch values cannot contain NUL");
        bytes += value.toUtf8().size();
        if (bytes > qsizetype{64} * 1024)
            throw std::invalid_argument("Launch values exceed 64 KiB");
    };
    check_text(launch.program);
    check_text(launch.directory);
    if (launch.arguments.size() > 256)
        throw std::invalid_argument("Launch has more than 256 arguments");
    for (const auto& argument : launch.arguments)
        check_text(argument);
    if (launch.program.isEmpty() || launch.directory.isEmpty())
        throw std::invalid_argument("Program and working directory are required");
    if (launch.size.columns == 0 || launch.size.rows == 0 ||
        static_cast<std::size_t>(launch.size.columns) * launch.size.rows >
            static_cast<std::size_t>(wire::max_cells))
        throw std::invalid_argument("Invalid initial terminal size");
    const QFileInfo directory(launch.directory);
    if (!directory.isDir())
        throw std::invalid_argument("Working directory does not exist");
    launch.directory = directory.canonicalFilePath();
    const QString resolved = launch.program.contains(QLatin1Char('/'))
                                 ? QDir(launch.directory).absoluteFilePath(launch.program)
                                 : QStandardPaths::findExecutable(launch.program);
    const QFileInfo program(resolved);
    if (resolved.isEmpty() || !program.isFile() || !program.isExecutable())
        throw std::invalid_argument("Program is not an executable file");
    // Preserve executable symlinks: multicall programs and shells can choose
    // behavior from argv[0] (for example, sh versus bash or BusyBox applets).
    launch.program = QDir::cleanPath(program.absoluteFilePath());
    return launch;
}

QString login_shell() {
#ifdef _WIN32
    // Windows session backends are not implemented; keep the last resort.
    return QStringLiteral("/bin/sh");
#else
    // POSIX: the account's shell field in the password database. getpwuid is
    // available on macOS and Linux, and avoids spawning a subprocess.
    // Bound an implausible sysconf result instead of allocating it.
    constexpr long kMaximumShellBuffer = 1024L * 1024L;
    long buffer_size = ::sysconf(_SC_GETPW_R_SIZE_MAX);
    if (buffer_size <= 0 || buffer_size > kMaximumShellBuffer)
        buffer_size = 16384;
    std::vector<char> buffer(static_cast<std::size_t>(buffer_size));
    passwd entry{};
    passwd* result = nullptr;
    if (::getpwuid_r(::getuid(), &entry, buffer.data(), buffer.size(), &result) == 0 &&
        result != nullptr && entry.pw_shell != nullptr) {
        const QString shell = QString::fromLocal8Bit(entry.pw_shell);
        // An empty or non-executable field is not usable; keep the last resort.
        if (!shell.isEmpty()) {
            const QFileInfo candidate(shell);
            if (candidate.isFile() && candidate.isExecutable())
                return shell;
        }
    }
    return QStringLiteral("/bin/sh");
#endif
}

LaunchSpec shell_launch(const QString& directory) {
    // Prefer the caller's environment, then the account's real login shell.
    // Falling back to /bin/sh gives macOS bash 3.2 in POSIX mode, which is not
    // the user's terminal and breaks agent CLIs that expect their normal PATH,
    // aliases and rc files. /bin/sh remains only a last resort.
    QString shell = qEnvironmentVariable("SHELL");
    if (shell.isEmpty())
        shell = login_shell();
    return validate_launch(
        {.program = shell, .arguments = {QStringLiteral("-i")}, .directory = directory});
}

QByteArray launch_fingerprint(const LaunchSpec& launch) {
    QByteArray bytes;
    QDataStream stream(&bytes, QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_6_0);
    stream.setByteOrder(QDataStream::BigEndian);
    stream << launch.program << launch.arguments << launch.directory;
    switch (launch.agent) {
    case AgentMode::terminal:
        return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
    case AgentMode::codex:
        bytes.prepend(QByteArrayLiteral("lapis-codex-v1\0"));
        return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
    case AgentMode::claude:
        bytes.prepend(QByteArrayLiteral("lapis-claude-v1\0"));
        return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
    }
    throw std::invalid_argument("Unknown agent integration mode");
}
} // namespace lapis::session
