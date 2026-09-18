#include "platform/posix/pty_process.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstring>
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
    qint64 pid{};
    int code{-1};
    QProcess::ExitStatus status{QProcess::NormalExit};
    bool exited{};
};
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
void observe(PtyProcess& process, QEventLoop& loop, Result& result) {
    QObject::connect(&process, &PtyProcess::started, &loop,
                     [&] { result.pid = process.processId(); });
    QObject::connect(&process, &PtyProcess::output, &loop,
                     [&result](const QByteArray& bytes) { result.output += bytes; });
    QObject::connect(&process, &PtyProcess::failure, &loop, [&](const QString& error) {
        result.failure = error;
        loop.quit();
    });
    QObject::connect(&process, &PtyProcess::finished, &loop,
                     [&](int code, QProcess::ExitStatus exit_status) {
                         result.code = code;
                         result.status = exit_status;
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
void signal_exit() {
    PtyProcess process;
    QEventLoop loop;
    Result result;
    observe(process, loop, result);
    QObject::connect(&process, &PtyProcess::started, &loop, [&] {
        QTimer::singleShot(20, &loop, [&] {
            require(::kill(static_cast<pid_t>(process.processId()), SIGTERM) == 0,
                    "Could not signal child");
        });
    });
    process.start({.program = QStringLiteral("/bin/sleep"),
                   .arguments = {QStringLiteral("30")},
                   .directory = QStringLiteral("/tmp")});
    loop.exec();
    require(result.exited && result.status == QProcess::CrashExit && result.failure.isEmpty(),
            "Signal exit status was not preserved");
}
void failed_start_reuse(const QString& directory) {
    const auto path = QDir(directory).filePath(QStringLiteral("invalid-interpreter"));
    QFile executable(path);
    require(executable.open(QIODevice::WriteOnly), "Cannot create invalid executable");
    executable.write("#!/nonexistent/lapis-interpreter\n");
    executable.close();
    require(executable.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner),
            "Cannot mark fixture executable");
    PtyProcess process;
    QEventLoop loop;
    Result result;
    observe(process, loop, result);
    process.start({.program = path, .arguments = {}, .directory = directory});
    require(process.writeBytes("stale-input\n"), "Starting queue rejected input");
    loop.exec();
    require(!result.failure.isEmpty(), "Failed exec was not reported");
    result = {};
    QObject::connect(&process, &PtyProcess::started, &loop, [&] {
        require(process.writeBytes("fresh-input\n"), "Restart input rejected");
    });
    QObject::connect(&process, &PtyProcess::output, &loop, [&](const QByteArray&) {
        if (result.output.contains("fresh-input"))
            loop.quit();
    });
    process.start({.program = QStringLiteral("/bin/cat"), .arguments = {}, .directory = directory});
    loop.exec();
    require(result.failure.isEmpty() && result.output.contains("fresh-input") &&
                !result.output.contains("stale-input"),
            "Failed exec leaked queued input");
}
void bounded_descendant_drain(const QString& directory) {
    QElapsedTimer elapsed;
    elapsed.start();
    const auto result = run({.program = QCoreApplication::applicationFilePath(),
                             .arguments = {QStringLiteral("--busy-child")},
                             .directory = directory});
    require(result.exited && result.code == 42 && result.failure.isEmpty() &&
                elapsed.elapsed() < 5000,
            "Descendant held the final drain open");
}
void cleanup_group(const QString& directory) {
    auto process = std::make_unique<PtyProcess>();
    QEventLoop loop;
    QByteArray output;
    QObject::connect(process.get(), &PtyProcess::output, &loop, [&](const QByteArray& bytes) {
        output += bytes;
        if (output.contains('\n'))
            loop.quit();
    });
    QTimer::singleShot(2000, &loop, &QEventLoop::quit);
    process->start({.program = QCoreApplication::applicationFilePath(),
                    .arguments = {QStringLiteral("--group-child")},
                    .directory = directory});
    loop.exec();
    const auto leader = static_cast<pid_t>(process->processId());
    const auto child = static_cast<pid_t>(output.trimmed().toInt());
    require(leader > 0 && child > 0 && ::getsid(child) == leader && ::getpgid(child) == leader,
            "Fixture child not in owned group");
    process.reset();
    QElapsedTimer elapsed;
    elapsed.start();
    while (::kill(child, 0) == 0 && elapsed.elapsed() < 2000)
        QThread::msleep(5);
    require(::kill(leader, 0) == -1 && errno == ESRCH, "Leader not reaped");
    require(::kill(child, 0) == -1 && errno == ESRCH, "Resistant same-group child survived");
}
void cleanup_after_leader_exit(const QString& directory) {
    const auto result = run({.program = QCoreApplication::applicationFilePath(),
                             .arguments = {QStringLiteral("--quiet-orphan")},
                             .directory = directory});
    const auto child = static_cast<pid_t>(result.output.trimmed().toInt());
    require(result.exited && result.code == 42 && result.failure.isEmpty() && child > 0,
            "Quiet descendant fixture failed");
    QElapsedTimer elapsed;
    elapsed.start();
    const auto leader = static_cast<pid_t>(result.pid);
    const auto absent = [](pid_t target) { return ::kill(target, 0) == -1 && errno == ESRCH; };
    // macOS can return EPERM while the dying group is still being reaped.
    // Only ESRCH proves disappearance; other results must keep waiting.
    while ((!absent(child) || !absent(-leader)) && elapsed.elapsed() < 2000)
        QThread::msleep(5);
    require(::kill(child, 0) == -1 && errno == ESRCH,
            "Quiet SIGHUP-resistant child survived normal leader exit");
    const int group_status = ::kill(-leader, 0);
    const int group_error = errno;
    if (group_status != -1 || group_error != ESRCH)
        throw std::runtime_error(
            "Process-group guard survived cleanup: leader=" + std::to_string(leader) +
            " child=" + std::to_string(child) + " status=" + std::to_string(group_status) +
            " errno=" + std::to_string(group_error) +
            " elapsed_ms=" + std::to_string(elapsed.elapsed()));
}
int descendant_fixture(bool busy, bool exit_leader = false) {
    std::array<int, 2> ready{};
    if (::pipe(ready.data()) != 0)
        return 1;
    const pid_t child = ::fork();
    if (child < 0)
        return 1;
    if (child == 0) {
        ::signal(SIGHUP, SIG_IGN);
        ::signal(SIGTERM, SIG_IGN);
        ::signal(SIGPIPE, SIG_IGN);
        ::close(ready[0]);
        if (::write(ready[1], "r", 1) != 1)
            ::_exit(1);
        ::close(ready[1]);
        if (busy) {
            std::array<char, 16384> bytes{};
            bytes.fill('x');
            while (::write(STDOUT_FILENO, bytes.data(), bytes.size()) > 0) {
            }
            ::_exit(0);
        }
        for (;;)
            ::pause();
    }
    ::close(ready[1]);
    char ready_byte{};
    if (::read(ready[0], &ready_byte, 1) != 1)
        return 1;
    ::close(ready[0]);
    if (busy) {
        ::usleep(50000);
        return 42;
    }
    std::cout << child << '\n' << std::flush;
    if (exit_leader)
        return 42;
    for (;;)
        ::pause();
}

} // namespace
int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--busy-child") == 0)
        return descendant_fixture(true);
    if (argc == 2 && std::strcmp(argv[1], "--group-child") == 0)
        return descendant_fixture(false);
    if (argc == 2 && std::strcmp(argv[1], "--quiet-orphan") == 0)
        return descendant_fixture(false, true);
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
        signal_exit();
        failed_start_reuse(directory.path());
        bounded_descendant_drain(directory.path());
        cleanup_group(directory.path());
        cleanup_after_leader_exit(directory.path());
        std::cout << "Literal argv, cwd, output drain, resize, exit, signal status, "
                     "restart, bounded drain and process-group cleanup passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
