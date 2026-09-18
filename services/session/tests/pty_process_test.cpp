#include "platform/posix/pty_process.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QTemporaryDir>
#include <QTimer>
#include <cerrno>
#include <csignal>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <unistd.h>

namespace {
using lapis::session::LaunchSpec;
using lapis::session::posix::PtyProcess;
struct Result {
    QByteArray output;
    QString failure;
    int code{-1};
    bool exited{};
};
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
void observe(PtyProcess& process, QEventLoop& loop, Result& result) {
    QObject::connect(&process, &PtyProcess::output, &loop,
                     [&result](const QByteArray& bytes) { result.output += bytes; });
    QObject::connect(&process, &PtyProcess::failure, &loop, [&](const QString& error) {
        result.failure = error;
        loop.quit();
    });
    QObject::connect(&process, &PtyProcess::finished, &loop, [&](int code) {
        result.code = code;
        result.exited = true;
        loop.quit();
    });
    QTimer::singleShot(5000, &loop, &QEventLoop::quit);
}
Result run(const LaunchSpec& launch) {
    PtyProcess process;
    QEventLoop loop;
    Result result;
    observe(process, loop, result);
    process.start(launch);
    if (result.failure.isEmpty() && !result.exited)
        loop.exec();
    return result;
}
void exact_arguments(const QString& directory) {
    const auto alias = QDir(directory).filePath(QStringLiteral("shell-alias"));
    require(QFile::link(QStringLiteral("/bin/sh"), alias), "Executable alias creation failed");
    const auto alias_result =
        run({.program = alias,
             .arguments = {QStringLiteral("-c"), QStringLiteral("printf '%s\\n' \"$0\"")},
             .directory = directory});
    require(alias_result.code == 0 && alias_result.output == alias.toUtf8() + "\r\n",
            "Executable symlink identity was changed");
    const auto result = run(
        {.program = QStringLiteral("/bin/sh"),
         .arguments = {QStringLiteral("-c"),
                       QStringLiteral("printf '%s\\n' \"$#\"; for value in \"$@\"; do printf "
                                      "'<%s>\\n' \"$value\"; done; test -f marker && echo cwd-ok"),
                       QStringLiteral("fixture"), QStringLiteral("literal space"),
                       QStringLiteral("meta &$|;<>"), QString{}},
         .directory = directory});
    require(result.failure.isEmpty() && result.exited && result.code == 0 &&
                result.output.contains("3\r\n<literal space>\r\n<meta &$|;<>>\r\n<>\r\ncwd-ok"),
            "Literal argv or cwd was changed");
}
void burst(const QString& directory) {
    const auto result =
        run({.program = QStringLiteral("/bin/sh"),
             .arguments = {QStringLiteral("-c"),
                           QStringLiteral("i=0; while [ $i -lt 12000 ]; do "
                                          "printf '0123456789abcdef\\n'; i=$((i+1)); done; "
                                          "printf 'END\\n'; exit 7")},
             .directory = directory});
    require(result.failure.isEmpty() && result.exited && result.code == 7 &&
                result.output.count("0123456789abcdef\r\n") == 12000 &&
                result.output.endsWith("END\r\n"),
            "Final output or exit status was lost");
}
void resize(const QString& directory) {
    PtyProcess process;
    QEventLoop loop;
    Result result;
    observe(process, loop, result);
    QObject::connect(&process, &PtyProcess::started, &loop, [&] {
        if (!process.resize({61, 17}) || !process.writeBytes("stty size; exit 0\n")) {
            result.failure = QStringLiteral("Resize/input failed");
            loop.quit();
        }
    });
    process.start({.program = QStringLiteral("/bin/sh"),
                   .arguments = {QStringLiteral("-i")},
                   .directory = directory});
    if (result.failure.isEmpty())
        loop.exec();
    require(result.failure.isEmpty() && result.exited && result.code == 0 &&
                result.output.contains("17 61\r\n"),
            "Interactive shell input/resize failed");
}
void cleanup(const QString& directory) {
    auto process = std::make_unique<PtyProcess>();
    QEventLoop loop;
    qint64 pid = 0;
    QObject::connect(process.get(), &PtyProcess::started, &loop, [&] {
        pid = process->processId();
        loop.quit();
    });
    QTimer::singleShot(2000, &loop, &QEventLoop::quit);
    process->start({.program = QStringLiteral("/bin/sleep"),
                    .arguments = {QStringLiteral("30")},
                    .directory = directory});
    loop.exec();
    require(pid > 0 && ::kill(static_cast<pid_t>(pid), 0) == 0, "Child was not started");
    process.reset();
    require(::kill(static_cast<pid_t>(pid), 0) == -1 && errno == ESRCH,
            "Destructor did not reap the owned child");
}
} // namespace
int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    try {
        QTemporaryDir directory;
        require(directory.isValid(), "Temporary directory failed");
        QFile marker(directory.filePath(QStringLiteral("marker")));
        require(marker.open(QIODevice::WriteOnly), "Marker creation failed");
        marker.close();
        exact_arguments(directory.path());
        burst(directory.path());
        resize(directory.path());
        require(!run({.program = QStringLiteral("/nonexistent/lapis-program"),
                      .arguments = {},
                      .directory = directory.path()})
                     .failure.isEmpty(),
                "Missing exec accepted");
        require(!run({.program = QStringLiteral("/bin/sh"),
                      .arguments = {},
                      .directory = directory.filePath(QStringLiteral("missing"))})
                     .failure.isEmpty(),
                "Missing cwd accepted");
        cleanup(directory.path());
        std::cout
            << "Literal argv, cwd, output drain, resize, exit, failures and child cleanup passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
