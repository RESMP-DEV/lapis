#include "terminal_surface.hpp"
#include "workspace.hpp"
#include <QCommandLineParser>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QProcess>
#include <QProcessEnvironment>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QScreen>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <sys/resource.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <libproc.h>
#endif
#include <vector>

namespace {
using lapis::desktop::SessionPreview;
using lapis::desktop::TerminalSurface;
namespace session = lapis::session;
quint64 now_ns() {
    return static_cast<quint64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    std::chrono::steady_clock::now().time_since_epoch())
                                    .count());
}
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
void until(const std::function<bool()>& condition, int timeout = 10000) {
    QElapsedTimer timer;
    timer.start();
    while (!condition()) {
        require(timer.elapsed() < timeout, "Probe event deadline expired");
        QCoreApplication::processEvents(QEventLoop::AllEvents, 2);
        QThread::msleep(1);
    }
}
QString screen_text(const session::TerminalSnapshot& snapshot) {
    QString text;
    for (std::size_t index = 0; index < snapshot.cells.size(); ++index) {
        const auto cell = snapshot.text(index);
        text += QString::fromUcs4(cell.data(), static_cast<qsizetype>(cell.size()));
    }
    return text;
}
struct KeyReceipt final : QObject {
    quint64 receipt{};
    bool eventFilter(QObject*, QEvent* event) override {
        if (event->type() == QEvent::KeyPress &&
            static_cast<QKeyEvent*>(event)->key() == Qt::Key_Return)
            receipt = now_ns();
        return false;
    }
};
struct OwnedService {
    QTemporaryDir directory{QStringLiteral("/tmp/lapis-latency-XXXXXX")};
    QProcess process;
    session::LaunchSpec launch;
    QString endpoint;
    OwnedService() {
        require(directory.isValid(), "Private fixture directory failed");
        endpoint = directory.filePath(QStringLiteral("session.sock"));
        // Echo is disabled: a marker is visible only after the child consumes
        // the terminal input, never from the tty line discipline's echo.
        const auto script = QStringLiteral(
            "import sys,termios\n"
            "a=termios.tcgetattr(0);a[3]&=~termios.ECHO;termios.tcsetattr(0,termios.TCSANOW,a)\n"
            "print('FIXTURE_READY',flush=True)\n"
            "for i,line in enumerate(sys.stdin,1):\n"
            " print('ACK%06d'%i,flush=True)\n");
        launch = session::validate_launch(
            {.program = QStandardPaths::findExecutable(QStringLiteral("python3")),
             .arguments = {QStringLiteral("-u"), QStringLiteral("-c"), script},
             .directory = directory.path()});
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("LAPIS_HISTORY_ROOT"),
                           directory.filePath(QStringLiteral("history")));
        process.setProcessEnvironment(environment);
        process.setStandardOutputFile(directory.filePath(QStringLiteral("service.log")));
        process.setStandardErrorFile(directory.filePath(QStringLiteral("service.log")),
                                     QIODevice::Append);
        QStringList arguments{endpoint, launch.directory, launch.program};
        arguments.append(launch.arguments);
        process.start(QStringLiteral(LAPIS_SESSION_SERVICE_PATH), arguments);
        require(process.waitForStarted(3000), "Controlled service failed to start");
        until(
            [this] { return QFile::exists(endpoint) || process.state() == QProcess::NotRunning; });
        require(process.state() != QProcess::NotRunning, "Controlled service exited");
    }
    ~OwnedService() {
        process.terminate();
        if (!process.waitForFinished(3000)) {
            process.kill();
            process.waitForFinished(3000);
        }
    }
};
QJsonObject distribution(std::vector<double> values) {
    require(!values.empty(), "Missing timing samples");
    std::sort(values.begin(), values.end());
    const auto percentile = [&values](double fraction) {
        const auto index =
            static_cast<std::size_t>(std::ceil(fraction * static_cast<double>(values.size()))) - 1;
        return values.at(index);
    };
    return {{QStringLiteral("p50_ms"), percentile(0.50)},
            {QStringLiteral("p95_ms"), percentile(0.95)},
            {QStringLiteral("p99_ms"), percentile(0.99)},
            {QStringLiteral("max_ms"), values.back()}};
}
double cpu_seconds() {
    rusage usage{};
    require(::getrusage(RUSAGE_SELF, &usage) == 0, "CPU usage unavailable");
    return static_cast<double>(usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) +
           static_cast<double>(usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1e6;
}
struct ProcessUsage {
    quint64 cpu_ns{};
    quint64 resident_bytes{};
    quint64 footprint_bytes{};
};
ProcessUsage process_usage(qint64 pid) {
#if defined(__APPLE__)
    rusage_info_v2 usage{};
    require(::proc_pid_rusage(static_cast<int>(pid), RUSAGE_INFO_V2,
                              reinterpret_cast<rusage_info_t*>(&usage)) == 0,
            "Process resource accounting unavailable");
    return {usage.ri_user_time + usage.ri_system_time, usage.ri_resident_size,
            usage.ri_phys_footprint};
#else
    static_cast<void>(pid);
    return {}; // Native process accounting must be qualified during the Linux port.
#endif
}
struct Connections {
    std::vector<QMetaObject::Connection> values;
    ~Connections() {
        for (const auto& connection : values)
            QObject::disconnect(connection);
    }
};
QJsonObject history_return_timing(SessionPreview& document, QQuickWindow& window) {
    std::atomic_bool requested{}, synchronized{};
    std::atomic<quint64> submitted{};
    Connections connections{{QObject::connect(
                                 &window, &QQuickWindow::afterSynchronizing, &window,
                                 [&] {
                                     if (requested.load())
                                         synchronized.store(true);
                                 },
                                 Qt::DirectConnection),
                             QObject::connect(
                                 &window, &QQuickWindow::frameSwapped, &window,
                                 [&] {
                                     if (requested.load() && synchronized.load()) {
                                         quint64 empty{};
                                         static_cast<void>(
                                             submitted.compare_exchange_strong(empty, now_ns()));
                                     }
                                 },
                                 Qt::DirectConnection)}};
    std::vector<double> samples;
    for (int index = 0; index < 20; ++index) {
        document.olderHistory();
        until([&] { return !document.historyRequestPending(); });
        require(document.historyActive() && document.historyMessage().isEmpty(),
                "No history page available for warm return measurement");
        synchronized.store(false);
        submitted.store(0);
        const auto start = now_ns();
        requested.store(true);
        document.returnToLive();
        until([&] { return submitted.load() != 0; });
        requested.store(false);
        samples.push_back(static_cast<double>(submitted.load() - start) / 1e6);
    }
    auto result = distribution(samples);
    result.insert(QStringLiteral("samples"), static_cast<int>(samples.size()));
    result.insert(
        QStringLiteral("scope"),
        QStringLiteral("Retained history-to-live screen return; not cross-session switching"));
    return result;
}
QString input_label(bool native) {
    return native ? QStringLiteral("OS-injected Return through AppKit/Qt; not physical keyboard")
                  : QStringLiteral("synthetic Qt Return");
}
QJsonObject run_probe(int samples, bool native) {
    OwnedService service;
    SessionPreview document(QStringLiteral("latency fixture"), service.directory.path(), {},
                            QColor(Qt::white), "");
    QQuickWindow window;
    window.setTitle(QStringLiteral("lapis controlled latency probe"));
    window.setGeometry(100, 100, 900, 540);
    TerminalSurface surface(window.contentItem());
    surface.setSize(QSizeF(900, 540));
    surface.setDocument(&document);
    surface.setInteractive(true);
    KeyReceipt key;
    surface.installEventFilter(&key);
    document.startLive(service.endpoint, service.launch, session::wire::AttachMode::discover);
    window.show();
    until([&] { return window.isExposed(); });
    window.requestActivate();
    surface.forceActiveFocus();
    until([&] {
        return window.isActive() && surface.hasActiveFocus() && document.inputReady() &&
               screen_text(document.snapshot()).contains(QStringLiteral("FIXTURE_READY"));
    });
    std::cerr << "Fixture ready and focused\n";
    require(window.rendererInterface()->graphicsApi() == QSGRendererInterface::Vulkan,
            "Probe requires Vulkan");
    std::atomic<quint64> wanted_revision{}, synced_revision{}, sync_ns{}, swapped_ns{};
    quint64 observed_sequence{};
    quint64 snapshot_ns{};
    QString marker;
    QVariantMap timing;
    const auto snapshot_connection =
        QObject::connect(&document, &SessionPreview::snapshotChanged, &window, [&] {
            if (marker.isEmpty() || !screen_text(document.snapshot()).contains(marker))
                return;
            const auto candidate = document.snapshotTiming();
            const auto sequence = candidate.value(QStringLiteral("sequence")).toULongLong();
            if (sequence <= observed_sequence)
                return;
            observed_sequence = sequence;
            timing = candidate;
            snapshot_ns = now_ns();
            wanted_revision.store(document.snapshot().revision);
        });
    const auto sync_connection = QObject::connect(
        &window, &QQuickWindow::afterSynchronizing, &window,
        [&] {
            const auto revision = wanted_revision.load();
            if (revision != 0 && synced_revision.load() != revision) {
                sync_ns.store(now_ns());
                synced_revision.store(revision);
            }
        },
        Qt::DirectConnection);
    const auto swap_connection = QObject::connect(
        &window, &QQuickWindow::frameSwapped, &window,
        [&] {
            const auto revision = wanted_revision.load();
            if (revision != 0 && synced_revision.load() == revision) {
                quint64 empty{};
                static_cast<void>(swapped_ns.compare_exchange_strong(empty, now_ns()));
            }
        },
        Qt::DirectConnection);
    Connections cleanup{{snapshot_connection, sync_connection, swap_connection}};
    const auto desktop_before = process_usage(::getpid());
    const auto service_before = process_usage(service.process.processId());
    std::vector<double> end_to_end, input_to_read, parsing, publishing, transport, rendering;
    QJsonArray raw;
    const auto refresh = window.screen()->refreshRate();
    int missed{};
    constexpr int warmup = 5;
    QProcess injection;
    const auto cliclick = QStandardPaths::findExecutable(QStringLiteral("cliclick"));
    require(!native || !cliclick.isEmpty(), "Native input probe needs cliclick");
    for (int index = 1; index <= samples + warmup; ++index) {
        wanted_revision.store(0);
        synced_revision.store(0);
        swapped_ns.store(0);
        sync_ns.store(0);
        key.receipt = 0;
        snapshot_ns = 0;
        marker = QStringLiteral("ACK%1").arg(index, 6, 10, QLatin1Char('0'));
        if (native) {
            const auto point = window.mapToGlobal(QPoint(450, 270));
            injection.start(cliclick, {QStringLiteral("-r"),
                                       QStringLiteral("c:%1,%2").arg(point.x()).arg(point.y()),
                                       QStringLiteral("kp:return")});
        } else {
            QKeyEvent press(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
            QCoreApplication::sendEvent(&surface, &press);
        }
        try {
            until([&] { return swapped_ns.load() != 0; });
        } catch (...) {
            std::cerr << "sample=" << index << " key=" << key.receipt << " snapshot=" << snapshot_ns
                      << " wanted=" << wanted_revision.load()
                      << " synced=" << synced_revision.load()
                      << " injector=" << injection.readAllStandardError().toStdString()
                      << " text=" << screen_text(document.snapshot()).toStdString().substr(0, 200)
                      << '\n';
            throw;
        }
        if (native) {
            until([&] { return injection.state() == QProcess::NotRunning; });
            require(injection.exitCode() == 0, "Native key injection failed");
        }
        const auto pty = timing.value(QStringLiteral("pty_read_ns")).toULongLong();
        const auto parse = timing.value(QStringLiteral("parse_end_ns")).toULongLong();
        const auto publish = timing.value(QStringLiteral("publish_ns")).toULongLong();
        const auto sync = sync_ns.load();
        const auto swap = swapped_ns.load();
        require(key.receipt && key.receipt <= pty && pty <= parse && parse <= publish &&
                    publish <= snapshot_ns && snapshot_ns <= sync && sync <= swap,
                "Missing or out-of-order correlated timing stages");
        if (index <= warmup)
            continue;
        const auto delta = [](quint64 later, quint64 earlier) {
            return static_cast<double>(later - earlier) / 1e6;
        };
        const auto elapsed = delta(swap, key.receipt);
        end_to_end.push_back(elapsed);
        input_to_read.push_back(delta(pty, key.receipt));
        parsing.push_back(delta(parse, pty));
        publishing.push_back(delta(publish, parse));
        transport.push_back(delta(snapshot_ns, publish));
        rendering.push_back(delta(swap, snapshot_ns));
        missed += static_cast<int>(elapsed > 1000.0 / refresh);
        raw.append(
            QJsonObject{{QStringLiteral("sequence"), static_cast<qint64>(observed_sequence)},
                        {QStringLiteral("revision"), static_cast<qint64>(wanted_revision.load())},
                        {QStringLiteral("input_to_frame_ms"), elapsed}});
    }
    marker.clear();
    wanted_revision.store(0);
    QObject::disconnect(snapshot_connection);
    QObject::disconnect(sync_connection);
    QObject::disconnect(swap_connection);
    const auto history_return =
        samples >= 30 ? history_return_timing(document, window) : QJsonObject{};
    // Sample idle separately from startup and active measurements.
    QElapsedTimer idle;
    idle.start();
    const auto cpu_start = cpu_seconds();
    const auto desktop_warm = process_usage(::getpid());
    const auto service_warm = process_usage(service.process.processId());
    std::atomic<quint64> idle_frames{};
    Connections idle_connection{{QObject::connect(
        &window, &QQuickWindow::frameSwapped, &window, [&] { ++idle_frames; },
        Qt::DirectConnection)}};
    QEventLoop idle_loop;
    QTimer::singleShot(5000, &idle_loop, &QEventLoop::quit);
    idle_loop.exec();
    const auto cpu =
        (cpu_seconds() - cpu_start) / (static_cast<double>(idle.elapsed()) / 1000.0) * 100.0;
    const auto desktop_after = process_usage(::getpid());
    const auto service_after = process_usage(service.process.processId());
    const auto service_cpu = static_cast<double>(service_after.cpu_ns - service_warm.cpu_ns) /
                             (static_cast<double>(idle.elapsed()) * 1e6) * 100.0;
    rusage usage{};
    require(::getrusage(RUSAGE_SELF, &usage) == 0, "Memory usage unavailable");
    return {{QStringLiteral("schema"), QStringLiteral("lapis.terminal-latency/1")},
            {QStringLiteral("input"), input_label(native)},
            {QStringLiteral("endpoint"),
             QStringLiteral("frameSwapped after the matching snapshot revision synchronized; "
                            "submission proxy, not pixel presentation")},
            {QStringLiteral("renderer"), QStringLiteral("Vulkan")},
            {QStringLiteral("qt"), QString::fromLatin1(qVersion())},
            {QStringLiteral("samples"), samples},
            {QStringLiteral("warmup"), warmup},
            {QStringLiteral("refresh_hz"), refresh},
            {QStringLiteral("over_one_refresh_interval"), missed},
            {QStringLiteral("input_to_frame"), distribution(end_to_end)},
            {QStringLiteral("history_to_live_frame"), history_return},
            {QStringLiteral("input_to_pty_read"), distribution(input_to_read)},
            {QStringLiteral("parse"), distribution(parsing)},
            {QStringLiteral("publication_wait"), distribution(publishing)},
            {QStringLiteral("transport"), distribution(transport)},
            {QStringLiteral("snapshot_to_frame"), distribution(rendering)},
            {QStringLiteral("desktop_idle_cpu_percent_one_core"), cpu},
            {QStringLiteral("service_idle_cpu_percent_one_core"), service_cpu},
            {QStringLiteral("idle_frames_submitted"), static_cast<qint64>(idle_frames.load())},
            {QStringLiteral("idle_seconds"), static_cast<double>(idle.elapsed()) / 1000.0},
            {QStringLiteral("desktop_resident_before_bytes"),
             static_cast<qint64>(desktop_before.resident_bytes)},
            {QStringLiteral("desktop_resident_warm_bytes"),
             static_cast<qint64>(desktop_warm.resident_bytes)},
            {QStringLiteral("desktop_resident_after_idle_bytes"),
             static_cast<qint64>(desktop_after.resident_bytes)},
            {QStringLiteral("desktop_warm_footprint_bytes"),
             static_cast<qint64>(desktop_warm.footprint_bytes)},
            {QStringLiteral("service_resident_before_bytes"),
             static_cast<qint64>(service_before.resident_bytes)},
            {QStringLiteral("service_resident_warm_bytes"),
             static_cast<qint64>(service_warm.resident_bytes)},
            {QStringLiteral("service_resident_after_idle_bytes"),
             static_cast<qint64>(service_after.resident_bytes)},
            {QStringLiteral("desktop_peak_rss_native_units"), static_cast<qint64>(usage.ru_maxrss)},
            {QStringLiteral("rss_units"), QStringLiteral("bytes on macOS; KiB on Linux")},
            {QStringLiteral("raw"), raw}};
}
} // namespace
int main(int argc, char** argv) {
    qputenv("QT_MTL_NO_TRANSACTION", "1");
    qputenv("QT_VULKAN_LIB", LAPIS_VULKAN_LIBRARY);
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Vulkan);
    QCoreApplication::setAttribute(Qt::AA_MacDontSwapCtrlAndMeta);
    QGuiApplication app(argc, argv);
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption(
        {QStringLiteral("output"), QStringLiteral("JSON receipt"), QStringLiteral("path")});
    parser.addOption({QStringLiteral("samples"), QStringLiteral("Measured samples"),
                      QStringLiteral("count"), QStringLiteral("100")});
    parser.addOption(
        {QStringLiteral("native"), QStringLiteral("Use OS-injected Return via cliclick")});
    parser.process(app);
    try {
        bool valid{};
        const int samples = parser.value(QStringLiteral("samples")).toInt(&valid);
        require(valid && samples >= 1 && samples <= 1000, "Sample count must be 1..1000");
        require(!parser.value(QStringLiteral("output")).isEmpty(), "An output path is required");
        const auto result = run_probe(samples, parser.isSet(QStringLiteral("native")));
        QFile file(parser.value(QStringLiteral("output")));
        require(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "Cannot write receipt");
        const auto bytes = QJsonDocument(result).toJson();
        require(file.write(bytes) == bytes.size(), "Incomplete receipt write");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
