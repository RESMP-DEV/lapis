#include "launch_spec.hpp"

#include "transport/local_protocol.hpp"
#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <stdexcept>

namespace lapis::session {
LaunchSpec validate_launch(LaunchSpec launch) {
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

LaunchSpec shell_launch(const QString& directory) {
    QString shell = qEnvironmentVariable("SHELL");
    if (shell.isEmpty())
        shell = QStringLiteral("/bin/sh");
    return validate_launch(
        {.program = shell, .arguments = {QStringLiteral("-i")}, .directory = directory});
}

QByteArray launch_fingerprint(const LaunchSpec& launch) {
    QByteArray bytes;
    QDataStream stream(&bytes, QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_6_0);
    stream.setByteOrder(QDataStream::BigEndian);
    stream << launch.program << launch.arguments << launch.directory;
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
}
} // namespace lapis::session
