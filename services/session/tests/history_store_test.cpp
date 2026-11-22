#include "history_store.hpp"
#include "history_worker.hpp"
#include "transport/local_protocol.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
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
    terminal.feed("\\unused\r\n\x1b[1;3;31m界é\x1b[0m\x1b]8;;https://example.com/"
                  "history\x1b\\link\x1b]8;;\x1b\\");
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
    // Exercise both wire array caps with varied colors, producing a
    // nontrivial compressed record even though the grapheme pool compresses.
    constexpr std::size_t cells = 32768;
    constexpr std::size_t codepoints = 65536;
    constexpr std::size_t rows = 2;
    const TerminalSize size{static_cast<std::uint16_t>(cells / rows), rows};
    TerminalSnapshot snapshot;
    snapshot.size = size;
    snapshot.graphemes = std::u32string(codepoints, U'x');
    snapshot.cells.resize(cells);
    // Reproducible compression fixture, not security-sensitive randomness.
    // NOLINTNEXTLINE(bugprone-random-generator-seed)
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
// Appended and read pages come back from RAM until their segment changes or
// leaves; the budgets still hold while most appends skip the archive scan.
void check_ram_cache() {
    QTemporaryDir directory(QStringLiteral("/tmp/lapis-history-cache-XXXXXX"));
    require(directory.isValid());
    const auto root = directory.filePath(QStringLiteral("archive"));
    const QString session(32, QLatin1Char('a'));
    const QString other(32, QLatin1Char('b'));
    const auto snapshot = page();
    {
        HistoryStore store(root, session);
        const auto id1 = store.append(snapshot);
        const auto id2 = store.append(snapshot);
        require(store.cachedPages() == 2 && store.cachedBytes() > 0);
        // Served from RAM: no disk read, and the same page the disk holds.
        auto cached = present(store.at(0));
        require(cached.id == id1 && store.cacheHits() == 1 && store.diskReads() == 0);
        require(present(store.older()).id == id2 && store.diskReads() == 0);
        HistoryStore reopened(root, session);
        auto disk = present(reopened.at(0));
        require(reopened.diskReads() == 1 && reopened.cacheHits() == 0);
        require(wire::encode_snapshot(disk.snapshot) == wire::encode_snapshot(cached.snapshot));
        // A page read from disk is kept: the second read is from RAM.
        require(present(reopened.at(0)).id == id1 && reopened.cacheHits() == 1 &&
                reopened.diskReads() == 1);
        // Another writer appending to the segment changes it: kept pages are
        // read and verified from disk again, then kept again.
        static_cast<void>(reopened.append(snapshot));
        require(present(store.at(0)).id == id1 && store.diskReads() == 1);
        require(present(store.at(0)).id == id1 && store.diskReads() == 1 && store.cacheHits() == 3);
        // A kept page from a corrupted segment is not trusted.
        {
            QFile file(segment_of(root, session, id1));
            require(file.open(QIODevice::ReadWrite) && file.seek(80) && file.write("Z") == 1);
        }
        rejects([&] { static_cast<void>(store.at(0)); });
        store.clear();
        require(store.cachedPages() == 0 && store.cachedBytes() == 0 && !store.older());
    }
    // The RAM budget evicts least recently used pages first.
    {
        quint64 payload{};
        {
            HistoryStore probe(root, QString(32, QLatin1Char('f')));
            static_cast<void>(probe.append(snapshot));
            payload = probe.cachedBytes();
            probe.clear();
        }
        // Room for two of these pages.
        HistoryStore store(root, session, {.cache_bytes = payload * 2});
        const auto id1 = store.append(snapshot);
        const auto id2 = store.append(snapshot);
        require(present(store.at(0)).id == id1); // id1 now the most recently used
        static_cast<void>(store.append(snapshot));
        require(store.cachedPages() == 2 && store.cachedBytes() == payload * 2);
        require(present(store.at(0)).id == id1 && store.diskReads() == 0);
        require(present(store.newer(id1)).id == id2 && store.diskReads() == 1);
        HistoryStore none(root, session, {.cache_bytes = 0});
        static_cast<void>(none.append(snapshot));
        require(none.cachedPages() == 0 && present(none.older()).id > id2 && none.diskReads() == 1);
        store.clear();
    }
    // Generous budgets: appends within a segment skip the archive scan.
    {
        HistoryStore store(root, other);
        for (int index = 0; index < 64; ++index)
            static_cast<void>(store.append(snapshot));
        require(store.budgetScans() <= 2 && store.cachedPages() == 64);
        store.clear();
    }
    // Tight budgets: every append that could matter scans, and both budgets
    // hold for this session, for another's eviction of it, and across reopen.
    {
        const auto bytes = record_bytes(root);
        const HistoryLimits limits{bytes * 4, bytes * 6};
        HistoryStore store(root, session, limits);
        for (int index = 0; index < 20; ++index) {
            static_cast<void>(store.append(snapshot));
            const auto stats = store.stats();
            require(stats.session_bytes <= limits.session_bytes &&
                    stats.global_bytes <= limits.global_bytes);
        }
        const auto oldest = present(store.at(0)).id;
        require(store.cachedPages() <= store.stats().pages);
        HistoryStore second(root, other, limits);
        for (int index = 0; index < 20; ++index)
            static_cast<void>(second.append(snapshot));
        require(second.stats().global_bytes <= limits.global_bytes);
        // The other session's budget took this one's older segments; their
        // kept pages are not served, and reading recovers to what remains.
        const auto remaining = present(store.at(0));
        require(remaining.id > oldest && remaining.snapshot.history.viewport_offset == 0);
        // One page per segment at this budget: each page read has its file.
        for (auto item = store.older(); item; item = store.older(item->id))
            require(item->id >= remaining.id &&
                    QFileInfo::exists(segment_of(root, session, item->id)));
    }
}
double milliseconds(qint64 nanoseconds) { return static_cast<double>(nanoseconds) / 1e6; }
// Prints timings against an archive under `root` (use a copy, never the live
// archive): every page of its largest session read from disk then from RAM,
// and appends of a new session with and without the archive scan.
int timing(const QString& root) {
    QString largest;
    qint64 largest_bytes{};
    int sessions{};
    for (const auto& name : QDir(root).entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (name.size() != 32)
            continue;
        ++sessions;
        qint64 bytes{};
        for (const auto& file : QDir(root + QLatin1Char('/') + name).entryInfoList(QDir::Files))
            bytes += file.size();
        if (bytes > largest_bytes) {
            largest_bytes = bytes;
            largest = name;
        }
    }
    require(!largest.isEmpty());
    QElapsedTimer clock;
    HistoryStore store(root, largest);
    const auto pages = store.stats().pages;
    std::vector<qint64> disk;
    std::vector<qint64> ram;
    for (int pass = 0; pass < 2; ++pass) {
        auto& times = pass == 0 ? disk : ram;
        quint64 row{};
        for (;;) {
            clock.start();
            auto item = store.at(row);
            times.push_back(clock.nsecsElapsed());
            if (!item ||
                item->snapshot.history.viewport_offset + item->snapshot.history.viewport_rows >=
                    item->snapshot.history.total_rows)
                break;
            row = item->snapshot.history.viewport_offset + item->snapshot.history.viewport_rows;
        }
    }
    const auto report = [](const char* label, std::vector<qint64> times) {
        std::sort(times.begin(), times.end());
        const auto at = [&](double q) {
            return milliseconds(times[static_cast<std::size_t>(q * double(times.size() - 1))]);
        };
        std::cout << label << ": n=" << times.size() << " p50=" << at(0.5) << " ms p95=" << at(0.95)
                  << " ms p99=" << at(0.99) << " ms max=" << at(1.0) << " ms\n";
    };
    std::cout << "archive: " << sessions << " sessions; largest " << largest_bytes << " bytes, "
              << pages << " pages\n";
    report("read, disk (first pass)", disk);
    report("read, RAM (second pass)", ram);
    std::cout << "disk reads " << store.diskReads() << ", RAM hits " << store.cacheHits()
              << ", RAM " << store.cachedBytes() << " bytes in " << store.cachedPages()
              << " pages\n";
    // Where a RAM read goes, on the newest page.
    {
        const auto newest = present(store.older());
        const auto encoded = wire::encode_snapshot(newest.snapshot);
        const auto payload = qCompress(encoded, 6);
        std::vector<qint64> inflate;
        std::vector<qint64> decode;
        std::vector<qint64> encode;
        for (int index = 0; index < 200; ++index) {
            clock.start();
            const auto bytes = qUncompress(payload);
            inflate.push_back(clock.nsecsElapsed());
            clock.start();
            const auto decoded = wire::decode_snapshot(bytes);
            decode.push_back(clock.nsecsElapsed());
            clock.start();
            static_cast<void>(wire::encode_snapshot(decoded));
            encode.push_back(clock.nsecsElapsed());
        }
        std::cout << "newest page " << newest.snapshot.size.columns << "x"
                  << newest.snapshot.size.rows << ", " << encoded.size() << " bytes encoded, "
                  << payload.size() << " compressed\n";
        report("  qUncompress", inflate);
        report("  wire::decode_snapshot", decode);
        report("  wire::encode_snapshot (service reply)", encode);
    }
    Terminal terminal({200, 50});
    for (int line = 0; line < 50; ++line)
        terminal.feed("agent output line " + std::to_string(line) +
                      " with enough varied text to compress like a real page\r\n");
    const auto sample = terminal.snapshot();
    HistoryStore writer(root, QString(32, QLatin1Char('9')));
    std::vector<qint64> plain;
    std::vector<qint64> scanned;
    for (int index = 0; index < 400; ++index) {
        const auto scans = writer.budgetScans();
        clock.start();
        static_cast<void>(writer.append(sample));
        (writer.budgetScans() != scans ? scanned : plain).push_back(clock.nsecsElapsed());
    }
    report("append, no scan", plain);
    if (!scanned.empty())
        report("append, with archive scan", scanned);
    writer.clear();
    return 0;
}
} // namespace
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        if (app.arguments().size() == 3 && app.arguments().at(1) == QStringLiteral("--timing"))
            return timing(app.arguments().at(2));
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
        check_ram_cache();
        check_worker_drain();
        check_worker_drain_deadline();
        std::cout << "History roundtrip, quotas, corruption and write recovery passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
