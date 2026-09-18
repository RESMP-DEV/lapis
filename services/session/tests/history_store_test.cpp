#include "history_store.hpp"
#include "history_worker.hpp"
#include "transport/local_protocol.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QTimer>
#include <csignal>
#include <iostream>
#include <source_location>
#include <stdexcept>
#include <sys/resource.h>
#include <unistd.h>
namespace {
using namespace lapis::session;
void require(bool condition, std::source_location where = std::source_location::current()) {
    if (!condition)
        throw std::runtime_error("History check failed at " + std::to_string(where.line()));
}
template <typename F> void rejects(F operation) {
    bool rejected{};
    try {
        operation();
    } catch (const std::exception&) {
        rejected = true;
    }
    require(rejected);
}
HistoryPage present(std::optional<HistoryPage> page) {
    if (!page)
        throw std::runtime_error("Expected history page was absent");
    return std::move(*page);
}
TerminalSnapshot page() {
    Terminal terminal({20, 4});
    terminal.feed("\\unused\r\n\x1b[1;3;31m界é\x1b[0m");
    auto result = terminal.snapshot();
    result.cursor = {};
    return result;
}
void check_worker_drain() {
    QTemporaryDir directory(QStringLiteral("/tmp/lapis-history-drain-XXXXXX"));
    require(directory.isValid());
    const auto root = directory.filePath(QStringLiteral("archive"));
    const QString session_id(32, QLatin1Char('d'));
    HistoryWorker worker(root, session_id, {});
    std::vector<TerminalSnapshot> pages(128, page());
    require(worker.append(std::move(pages)));
    QEventLoop loop;
    bool drained{};
    bool failed{};
    worker.failure = [&](const QString&) { failed = true; };
    worker.drain([&] {
        drained = worker.idle();
        loop.quit();
    });
    require(!worker.append({page()}));
    require(!worker.read({}, {}));
    QTimer::singleShot(5000, &loop, &QEventLoop::quit);
    loop.exec();
    require(drained && !failed);
    HistoryStore store(root, session_id);
    std::size_t count{};
    for (auto item = store.older(); item; item = store.older(item->id))
        ++count;
    require(count == 128);
}
void check_worker_drain_deadline() {
    QTemporaryDir directory(QStringLiteral("/tmp/lapis-history-deadline-XXXXXX"));
    require(directory.isValid());
    HistoryWorker worker(directory.filePath(QStringLiteral("archive")),
                         QString(32, QLatin1Char('e')), {});
    require(worker.append(std::vector<TerminalSnapshot>(128, page())));
    QEventLoop loop;
    int completions{};
    bool pending_at_deadline{};
    worker.drain(
        [&] {
            ++completions;
            pending_at_deadline = !worker.idle();
            loop.quit();
        },
        0);
    QTimer::singleShot(5000, &loop, &QEventLoop::quit);
    loop.exec();
    require(completions == 1 && pending_at_deadline);
    require(!worker.append({page()}));
}
void check_store() {
    QTemporaryDir directory(QStringLiteral("/tmp/lapis-history-XXXXXX"));
    require(directory.isValid());
    const auto root = directory.filePath(QStringLiteral("archive"));
    const QString first(32, QLatin1Char('a'));
    const QString second(32, QLatin1Char('b'));
    const auto snapshot = page();
    const auto bytes = static_cast<quint64>(wire::encode_snapshot(snapshot).size() + 48);
    const HistoryLimits limits{bytes * 2, bytes * 3, 4};
    HistoryStore store(root, first, limits);
    const auto id1 = store.append(snapshot);
    const auto id2 = store.append(snapshot);
    const auto id3 = store.append(snapshot);
    require(id1 < id2 && id2 < id3);
    require(store.stats().session_bytes == bytes * 2);
    require(present(store.older()).id == id3);
    require(present(store.older(id3)).id == id2 && !store.older(id2));
    require(present(store.newer(id1)).id == id2 && !store.newer(id3));
    require(wire::encode_snapshot(present(store.older()).snapshot) ==
            wire::encode_snapshot(snapshot));
    HistoryStore reopened(root, first, limits);
    require(present(reopened.older()).id == id3);
    HistoryStore other(root, second, limits);
    static_cast<void>(other.append(snapshot));
    static_cast<void>(other.append(snapshot));
    require(other.stats().global_bytes <= limits.global_bytes);
    store.clear();
    require(!store.older());
    const auto after_clear = store.append(snapshot);
    require(after_clear > id3);
    const auto path = root + QLatin1Char('/') + first +
                      QStringLiteral("/%1.page").arg(after_clear, 20, 10, QLatin1Char('0'));
    {
        QFile file(path);
        require(file.open(QIODevice::ReadWrite));
        require(file.seek(file.size() - 1));
        require(file.write("X") == 1);
    }
    rejects([&] { static_cast<void>(store.older()); });
    store.clear();
    const auto truncated = store.append(snapshot);
    {
        QFile file(root + QLatin1Char('/') + first +
                   QStringLiteral("/%1.page").arg(truncated, 20, 10, QLatin1Char('0')));
        require(file.open(QIODevice::ReadWrite) && file.resize(12));
    }
    rejects([&] { static_cast<void>(reopened.older()); });
    store.clear();
    QFile partial(root + QLatin1Char('/') + first + QStringLiteral("/.unfinished.tmp"));
    require(partial.open(QIODevice::WriteOnly) && partial.write("partial") == 7);
    partial.close();
    require(!store.older());
    require(partial.exists());
    QFile interrupted(root + QLatin1Char('/') + first + QStringLiteral("/.pending"));
    require(interrupted.open(QIODevice::WriteOnly));
    require(interrupted.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner));
    require(interrupted.write(QByteArray(4096, 'x')) == 4096);
    interrupted.close();
    require(!store.older());
    require(!interrupted.exists());
    rejects([&] { HistoryStore invalid(root, first, {0, 1024, 4}); });
    rejects([&] { HistoryStore invalid(root, QStringLiteral("../escape"), limits); });
    const auto linked = directory.filePath(QStringLiteral("linked"));
    require(::symlink(QFile::encodeName(root).constData(), QFile::encodeName(linked).constData()) ==
            0);
    rejects([&] { HistoryStore invalid(linked, first, limits); });
    // Real short-write failure under a per-process file-size quota. This is
    // EFBIG, not a claim that the host filesystem itself ran out of blocks.
    const auto preserved = store.append(snapshot);
    QProcess child;
    child.start(QCoreApplication::applicationFilePath(),
                {QStringLiteral("--quota-child"), root, first});
    require(child.waitForFinished(10000));
    require(child.exitStatus() == QProcess::NormalExit && child.exitCode() == 0);
    require(present(store.older()).id == preserved);
    require(store.append(snapshot) > preserved);
    QProcess writer_a, writer_b;
    writer_a.start(QCoreApplication::applicationFilePath(),
                   {QStringLiteral("--writer"), root, first});
    writer_b.start(QCoreApplication::applicationFilePath(),
                   {QStringLiteral("--writer"), root, second});
    require(writer_a.waitForFinished(10000) && writer_b.waitForFinished(10000));
    require(writer_a.exitStatus() == QProcess::NormalExit && writer_a.exitCode() == 0);
    require(writer_b.exitStatus() == QProcess::NormalExit && writer_b.exitCode() == 0);
    require(store.stats().global_bytes <= limits.global_bytes);
}
} // namespace
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        if (app.arguments().size() == 4 && app.arguments().at(1) == QStringLiteral("--writer")) {
            const auto snapshot = page();
            const auto bytes = static_cast<quint64>(wire::encode_snapshot(snapshot).size() + 48);
            HistoryStore store(app.arguments().at(2), app.arguments().at(3),
                               {bytes * 2, bytes * 3, 4});
            for (int index = 0; index < 20; ++index)
                static_cast<void>(store.append(snapshot));
            return 0;
        }
        if (app.arguments().size() == 4 &&
            app.arguments().at(1) == QStringLiteral("--quota-child")) {
            const auto snapshot = page();
            HistoryStore store(app.arguments().at(2), app.arguments().at(3));
            std::signal(SIGXFSZ, SIG_IGN);
            const rlimit quota{512, 512};
            require(::setrlimit(RLIMIT_FSIZE, &quota) == 0);
            rejects([&] { static_cast<void>(store.append(snapshot)); });
            return 0;
        }
        check_store();
        check_worker_drain();
        check_worker_drain_deadline();
        std::cout << "History roundtrip, quotas, corruption and write recovery passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
