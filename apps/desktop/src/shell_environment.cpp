#include "shell_environment.hpp"

#include "launch_spec.hpp"

#include <QDebug>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <algorithm>
#include <array>
#include <unistd.h>

namespace lapis::desktop {
namespace {
constexpr std::array kSkipped{"PWD", "OLDPWD", "SHLVL", "_", "TERM"};

#ifdef Q_OS_MACOS
constexpr int kShellTimeoutMs = 8000;

void adopt_from_shell() {
    QString shell = qEnvironmentVariable("SHELL");
    if (shell.isEmpty())
        shell = session::login_shell();
    QProcess process;
    process.setProgram(shell);
    process.setArguments({QStringLiteral("-i"), QStringLiteral("-l"), QStringLiteral("-c"),
                          QStringLiteral("printf '\\n%1\\n'; exec /usr/bin/env -0")
                              .arg(QLatin1String(kEnvironmentMarker))});
    process.setStandardInputFile(QProcess::nullDevice());
    process.setStandardErrorFile(QProcess::nullDevice());
    process.start();
    if (!process.waitForFinished(kShellTimeoutMs)) {
        process.kill();
        process.waitForFinished();
        qWarning().noquote() << "Could not read the login shell's environment from" << shell;
        return;
    }
    const auto variables = parse_environment(process.readAllStandardOutput());
    for (const auto& [name, value] : variables)
        qputenv(name.constData(), value);
    if (variables.isEmpty())
        qWarning().noquote() << "The login shell" << shell << "reported no environment";
    else
        qInfo().noquote() << "Using the environment of the login shell" << shell << "with"
                          << variables.size() << "variables and PATH"
                          << qEnvironmentVariable("PATH");
}
#endif
} // namespace

QList<std::pair<QByteArray, QByteArray>> parse_environment(const QByteArray& output) {
    QList<std::pair<QByteArray, QByteArray>> variables;
    const QByteArray line = '\n' + QByteArray(kEnvironmentMarker) + '\n';
    const auto start = output.lastIndexOf(line);
    if (start < 0)
        return variables;
    for (const auto& entry : output.mid(start + line.size()).split('\0')) {
        const auto equals = entry.indexOf('=');
        if (equals <= 0)
            continue;
        const QByteArray name = entry.left(equals);
        if (std::find(kSkipped.begin(), kSkipped.end(), name) != kSkipped.end())
            continue;
        variables.append({name, entry.mid(equals + 1)});
    }
    return variables;
}

void adopt_login_environment() {
#ifdef Q_OS_MACOS
    // launchd (process 1) starts apps from Finder and the Dock, and LaunchAgents.
    if (::getppid() == 1)
        adopt_from_shell();
#endif
}
} // namespace lapis::desktop
