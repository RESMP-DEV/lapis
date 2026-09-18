#include "platform/posix/pty_process.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QTimer>
#include <iostream>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    lapis::session::posix::PtyProcess process;
    QEventLoop loop;
    QByteArray output;
    bool finished = false;
    bool failed = false;
    QObject::connect(&process, &lapis::session::posix::PtyProcess::output, &loop,
                     [&](const QByteArray& bytes) { output += bytes; });
    QObject::connect(&process, &lapis::session::posix::PtyProcess::started, &loop, [&] {
        if (!process.resize({61, 17}) ||
            !process.writeBytes("stty -echo; stty size; printf 'BEGIN_BURST\\n'; i=0; while [ $i "
                                "-lt 12000 ]; do printf '0123456789abcdef\\n'; i=$((i+1)); done; "
                                "printf 'END_BURST\\n'; exit 0\n")) {
            failed = true;
            loop.quit();
        }
    });
    QObject::connect(&process, &lapis::session::posix::PtyProcess::failure, &loop,
                     [&](const QString& error) {
                         std::cerr << error.toStdString();
                         failed = true;
                         loop.quit();
                     });
    QObject::connect(&process, &lapis::session::posix::PtyProcess::finished, &loop, [&](int code) {
        finished = code == 0;
        loop.quit();
    });
    QTimer::singleShot(5000, &loop, &QEventLoop::quit);
    process.start(
        {.shell = QStringLiteral("/bin/sh"), .directory = QDir::currentPath(), .size = {80, 24}});
    loop.exec();
    if (failed || !finished || !output.contains("17 61") ||
        output.count("0123456789abcdef\r\n") != 12000 || !output.contains("END_BURST\r\n")) {
        std::cerr << "PTY launch/resize/exit failed: " << output.toStdString() << '\n';
        return 1;
    }
    lapis::session::posix::PtyProcess missing;
    QEventLoop error_loop;
    bool rejected = false;
    QObject::connect(&missing, &lapis::session::posix::PtyProcess::failure, &error_loop,
                     [&](const QString&) {
                         rejected = true;
                         error_loop.quit();
                     });
    QTimer::singleShot(2000, &error_loop, &QEventLoop::quit);
    missing.start({.shell = QStringLiteral("/nonexistent/lapis-shell"),
                   .directory = QDir::currentPath(),
                   .size = {80, 24}});
    error_loop.exec();
    if (!rejected) {
        std::cerr << "Failed exec was not reported\n";
        return 1;
    }
    std::cout << "PTY I/O, resize, child exit and failed exec passed\n";
}
