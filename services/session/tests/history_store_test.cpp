#include "history_store.hpp"
#include "history_worker.hpp"
#include "transport/local_protocol.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QTemporaryDir>
#include <QTimer>
#include <array>
#include <csignal>
#include <iostream>
#include <random>
#include <source_location>
#include <stdexcept>
#include <string>
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
// The bytes one page takes on disk, measured in a scratch store.
quint64 record_bytes(const QString& root) {
    HistoryStore scratch(root, QString(32, QLatin1Char('f')));
    static_cast<void>(scratch.append(page()));
    const auto bytes = scratch.stats().session_bytes;
    scratch.clear();
    return bytes;
}
QString segment_of(const QString& root, const QString& session, quint64 first) {
    return root + QLatin1Char('/') + session +
           QStringLiteral("/%1.seg").arg(first, 20, 10, QLatin1Char('0'));
}
void check_store() {
    QTemporaryDir directory(QStringLiteral("/tmp/lapis-history-XXXXXX"));
    require(directory.isValid());
    const auto root = directory.filePath(QStringLiteral("archive"));
    const QString first(32, QLatin1Char('a'));
    const QString second(32, QLatin1Char('b'));
    const auto snapshot = page();
    const auto bytes = record_bytes(root);
    const HistoryLimits limits{bytes * 2, bytes * 3};
    HistoryStore store(root, first, limits);
    const auto id1 = store.append(snapshot);
    const auto id2 = store.append(snapshot);
    const auto id3 = store.append(snapshot);
    require(id1 < id2 && id2 < id3);
    require(store.stats().session_bytes == bytes * 2);
    require(present(store.older()).id == id3);
    require(present(store.older(id3)).id == id2 && !store.older(id2));
    require(present(store.newer(id1)).id == id2 && !store.newer(id3));
    // Pages keep their cells and say where they sit among the kept rows.
    auto newest = present(store.older()).snapshot;
    const auto rows = std::size_t{snapshot.size.rows};
    require(newest.history.total_rows == rows * 2 && newest.history.viewport_offset == rows &&
            newest.history.viewport_rows == rows);
    newest.history = snapshot.history;
    require(wire::encode_snapshot(newest) == wire::encode_snapshot(snapshot));
    require(present(store.at(0)).id == id2 && present(store.at(rows - 1)).id == id2 &&
            present(store.at(rows)).id == id3 && present(store.at(rows * 100)).id == id3);
    HistoryStore reopened(root, first, limits);
    require(present(reopened.older()).id == id3 && present(reopened.at(0)).id == id2);
    HistoryStore other(root, second, limits);
    static_cast<void>(other.append(snapshot));
    static_cast<void>(other.append(snapshot));
    require(other.stats().global_bytes <= limits.global_bytes);
    // The other session's budget took this one's oldest segment. Stats report
    // the surviving archive without waiting for a read to reload the index.
    require(store.stats().pages == 1);
    // Reading on finds what remains.
    require(present(store.older()).id == id3 && !store.older(id3));
    store.clear();
    require(!store.older() && !store.at(0));
    const auto after_clear = store.append(snapshot);
    require(after_clear > id3);
    {
        QFile file(segment_of(root, first, after_clear));
        require(file.open(QIODevice::ReadWrite));
        require(file.seek(file.size() - 1));
        require(file.write("X") == 1);
    }
    rejects([&] { static_cast<void>(store.older()); });
    store.clear();
    // An interrupted write leaves a short record at a segment's end: the page
    // it held cannot be read, and the next store cuts it off.
    const auto truncated = store.append(snapshot);
    {
        QFile file(segment_of(root, first, truncated));
        require(file.open(QIODevice::ReadWrite) && file.resize(12));
    }
    rejects([&] { static_cast<void>(store.older()); });
    {
        HistoryStore recovered(root, first, limits);
        require(!recovered.older() && !QFileInfo::exists(segment_of(root, first, truncated)));
    }
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
    {
        HistoryStore opened(root, first, limits);
        require(!opened.older() && !interrupted.exists());
    }
    rejects([&] { HistoryStore invalid(root, first, {0, 1024}); });
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
                   {QStringLiteral("--writer"), root, first, QString::number(bytes)});
    writer_b.start(QCoreApplication::applicationFilePath(),
                   {QStringLiteral("--writer"), root, second, QString::number(bytes)});
    require(writer_a.waitForFinished(10000) && writer_b.waitForFinished(10000));
    require(writer_a.exitStatus() == QProcess::NormalExit && writer_a.exitCode() == 0);
    require(writer_b.exitStatus() == QProcess::NormalExit && writer_b.exitCode() == 0);
    require(store.stats().global_bytes <= limits.global_bytes);
}
// Thousands of pages stay a few segment files, and any row is one read away.
void check_long_history() {
    QTemporaryDir directory(QStringLiteral("/tmp/lapis-history-long-XXXXXX"));
    require(directory.isValid());
    const auto root = directory.filePath(QStringLiteral("archive"));
    const QString session(32, QLatin1Char('c'));
    HistoryStore store(root, session);
    Terminal terminal({80, 24});
    for (int line = 0; line < 24; ++line)
        terminal.feed("a line of agent output that repeats with small changes " +
                      std::to_string(line) + "\r\n");
    const auto sample = terminal.snapshot();
    constexpr int pages = 1000;
    quint64 first_id{};
    for (int index = 0; index < pages; ++index) {
        const auto id = store.append(sample);
        if (index == 0)
            first_id = id;
    }
    const auto stats = store.stats();
    // Compressed at least four times over the page as the wire carries it.
    const auto raw = static_cast<quint64>(wire::encode_snapshot(sample).size());
    require(stats.pages == pages && stats.session_bytes * 4U < quint64{pages} * raw);
    QDir folder(root + QLatin1Char('/') + session);
    require(folder.entryList({QStringLiteral("*.seg")}, QDir::Files).size() <= 2);
    const auto start = present(store.at(0));
    require(start.id == first_id && start.snapshot.history.viewport_offset == 0 &&
            start.snapshot.history.total_rows == std::size_t{pages} * 24U);
    const auto middle = present(store.at(std::size_t{pages} * 12U));
    require(middle.id == first_id + pages / 2 && middle.snapshot.size.columns == 80);
    HistoryStore reopened(root, session);
    require(present(reopened.at(std::size_t{pages} * 12U)).id == middle.id);
}
// Reach both v6 array caps with pseudorandom colors at the history boundary;
// its compressed record and reopened header agree on the committed length.
void check_maximum_history_page() {
    QTemporaryDir directory(QStringLiteral("/tmp/lapis-history-maximum-XXXXXX"));
    require(directory.isValid());
    const auto root = directory.filePath(QStringLiteral("archive"));
    const QString session(32, QLatin1Char('a'));
    // The wire's two array caps, filled with pseudorandom colors: this is the
    // reachable encoded-size boundary that compression cannot shrink.
    constexpr std::size_t cells = 32768;
    constexpr std::size_t codepoints = 65536;
    constexpr std::size_t rows = 2;
    const TerminalSize size{static_cast<std::uint16_t>(cells / rows), rows};
    TerminalSnapshot snapshot;
    snapshot.size = size;
    snapshot.graphemes = std::u32string(codepoints, U'x');
    snapshot.cells.resize(cells);
    std::mt19937 random{0x61706973U};
    std::uniform_int_distribution<std::uint16_t> byte_distribution{0, 255};
    std::array<std::uint8_t, 3> entropy{};
    for (std::size_t index = 0; index < cells; ++index) {
        for (auto& byte : entropy)
            byte = static_cast<std::uint8_t>(byte_distribution(random));
        const auto value = static_cast<std::uint32_t>(
            (std::uint32_t{entropy[0]} << 16U) | (std::uint32_t{entropy[1]} << 8U) | entropy[2]);
        auto& cell = snapshot.cells[index];
        cell.text_offset = index % codepoints;
        cell.text_length = codepoints - cell.text_offset;
        cell.style.foreground = {ColorKind::rgb, value};
        cell.style.background = {ColorKind::rgb, value ^ 0xffffffU};
        cell.style.underline_color = {ColorKind::rgb, value ^ 0x555555U};
    }
    constexpr qint64 record_limit = qint64{8} * 1024 * 1024;
    const auto encoded = wire::encode_snapshot(snapshot);
    const auto payload = qCompress(encoded, 6);
    require(encoded.size() <= record_limit && payload.size() <= record_limit);
    HistoryStore store(root, session);
    const auto id = store.append(snapshot);
    const auto stats = store.stats();
    require(stats.pages == 1 && stats.session_bytes == stats.global_bytes);
    require(present(store.at(0)).id == id);
    HistoryStore reopened(root, session);
    require(present(reopened.at(0)).id == id);
}
} // namespace
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        if (app.arguments().size() == 5 && app.arguments().at(1) == QStringLiteral("--writer")) {
            const auto snapshot = page();
            const auto bytes = app.arguments().at(4).toULongLong();
            HistoryStore store(app.arguments().at(2), app.arguments().at(3),
                               {bytes * 2, bytes * 3});
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
        check_long_history();
        check_maximum_history_page();
        check_worker_drain();
        check_worker_drain_deadline();
        std::cout << "History roundtrip, quotas, corruption and write recovery passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
