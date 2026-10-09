// Opt-in measurement of agent and category switching, not a CTest timing gate.
//
// Starts disposable agents (a stand-in CLI that draws a screen of text and
// waits) under the real session service in a private LAPIS_HOME, loads the
// real Main.qml offscreen with the software scene graph, and switches agents
// and categories through the keymap's own bindings, delivered as window-system
// key events so Qt's shortcut map, the QML handlers, the workspace and, when
// asked, the interaction log's event filter all run as in the app. Clicks on
// strip cards are measured the same way, and pointer moves on their own.
//
// Each sample is GUI-thread time from the input to the end of its delivery,
// to the end of the next frame's scene-graph sync and to that frame's
// frameSwapped. Offscreen/software rendering is not GPU presentation; compare
// runs of this tool with each other.
#include "keymap.hpp"
#include "terminal_surface.hpp"
#include "ui_preview.hpp"
#include "workspace.hpp"
#if __has_include("interaction_recorder.hpp")
#include "interaction_log.hpp"
#include "interaction_recorder.hpp"
#include "platform_desktop.hpp"
#define LAPIS_BENCH_INTERACTION_LOG 1
#endif

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeySequence>
#include <QProcess>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QThread>
#include <qpa/qwindowsysteminterface.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {
using lapis::desktop::SessionPreview;
using lapis::desktop::Workspace;

double now_ms() {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
void require(bool condition, const std::string& message) {
    if (!condition)
        throw std::runtime_error(message);
}
void pump(int milliseconds) {
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < milliseconds) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 2);
        QThread::msleep(1);
    }
}
bool wait_for(const std::function<bool()>& condition, int timeout) {
    QElapsedTimer clock;
    clock.start();
    while (!condition()) {
        if (clock.elapsed() >= timeout)
            return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
    return true;
}
void until(const std::function<bool()>& condition, int timeout, const char* what) {
    require(wait_for(condition, timeout), std::string("timed out waiting for ") + what);
}
QQuickItem* find_visual(QQuickItem* parent, const QString& name) {
    if (parent == nullptr)
        return nullptr;
    if (parent->objectName() == name && parent->isVisible())
        return parent;
    for (auto* child : parent->childItems())
        if (auto* match = find_visual(child, name))
            return match;
    return nullptr;
}
QString title(const SessionPreview* item) { return item != nullptr ? item->title() : QString(); }

// A stand-in CLI: a screen of distinct, colored text, then a quiet wait. It
// remains this script process, so cleanup can identify it by the private home.
void write_agent(const QString& path) {
    QFile file(path);
    require(file.open(QIODevice::WriteOnly), "write stand-in agent");
    file.write(R"py(#!/usr/bin/env python3
import sys
import time

for index in range(120):
    sys.stdout.write(
        f"\033[3{index % 7 + 1}m{index:04d}\033[0m agent output line "
        "with some text to shape and draw\n"
    )
sys.stdout.write("\033[1m> \033[0m")
sys.stdout.flush()
while True:
    time.sleep(1)
)py");
    file.close();
    QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
}

struct Stats {
    std::vector<double> dispatch; // the input delivered
    std::vector<double> synced;   // the next frame's polish and scene-graph sync done
    std::vector<double> frame;    // that frame swapped (software rasterizing included)
    int unchanged{};
    int timeouts{};
    void add(double delivered, double sync, double swapped) {
        dispatch.push_back(delivered);
        synced.push_back(sync);
        frame.push_back(swapped);
    }
    void dropLast() {
        dispatch.pop_back();
        synced.pop_back();
        frame.pop_back();
    }
};
double percentile(std::vector<double> values, double share) {
    if (values.empty())
        return 0;
    std::sort(values.begin(), values.end());
    const auto index =
        static_cast<std::size_t>(std::lround(share * static_cast<double>(values.size() - 1)));
    return values.at(std::min(index, values.size() - 1));
}
QJsonObject summary(const std::vector<double>& values) {
    double mean = 0;
    for (const auto value : values)
        mean += value;
    mean = values.empty() ? 0 : mean / static_cast<double>(values.size());
    return {{"n", static_cast<qint64>(values.size())},
            {"p50", percentile(values, 0.50)},
            {"p95", percentile(values, 0.95)},
            {"p99", percentile(values, 0.99)},
            {"max", values.empty() ? 0 : *std::max_element(values.begin(), values.end())},
            {"mean", mean}};
}
QJsonObject report(const Stats& stats) {
    return {{"dispatch", summary(stats.dispatch)},
            {"synced", summary(stats.synced)},
            {"frame", summary(stats.frame)},
            {"unchanged", stats.unchanged},
            {"timeouts", stats.timeouts}};
}

struct Options {
    int agents{};
    int categories{};
    int switches{};
    int gap{};
    int warmup{};
    bool log{};
    bool trace{};
    QString output;
};

// The workspace with its disposable agents: per category, the agents' ids.
std::vector<std::vector<QString>> start_agents(Workspace& workspace, const Options& options,
                                               const QString& folder) {
    std::vector<std::vector<QString>> agents;
    for (int category = 0; category < options.categories; ++category) {
        if (category > 0)
            require(workspace.addCategory(QStringLiteral("Bench %1").arg(category)),
                    "add category");
        agents.emplace_back();
        const int here = options.agents / options.categories +
                         (category < options.agents % options.categories ? 1 : 0);
        for (int index = 0; index < here; ++index) {
            require(workspace.createAgent(folder,
                                          QStringLiteral("agent %1.%2").arg(category).arg(index),
                                          QStringLiteral("opencode")),
                    "create agent: " + workspace.workspaceError().toStdString());
            auto* const created = workspace.focusedSession();
            require(created != nullptr, "new agent focused");
            agents.back().push_back(created->sessionId());
        }
    }
    until(
        [&workspace] {
            const auto sessions = workspace.sessions();
            return std::all_of(sessions.cbegin(), sessions.cend(), [](const QVariant& value) {
                return value.value<SessionPreview*>()->inputReady();
            });
        },
        30000, "agents ready");
    return agents;
}

// Every other category holds tiles: its first agent with two beside it.
void tile_categories(Workspace& workspace, const std::vector<std::vector<QString>>& agents) {
    for (std::size_t category = 0; category < agents.size(); category += 2) {
        const auto& ids = agents.at(category);
        if (ids.size() < 3)
            continue;
        require(workspace.selectSession(ids[0]), "select tile owner");
        require(workspace.tileSession(ids[1], ids[0], QStringLiteral("right")), "tile right");
        require(workspace.tileSession(ids[2], ids[1], QStringLiteral("bottom")), "tile below");
    }
}

class Bench : public QObject {
  public:
    Bench(Workspace& workspace, const lapis::desktop::KeyMap& keymap, QQuickWindow& window,
          const Options& options)
        : QObject(nullptr), workspace_(workspace), keymap_(keymap), window_(window),
          options_(options) {
        QObject::connect(&window, &QQuickWindow::frameSwapped, this,
                         [this] { swaps_.fetch_add(1, std::memory_order_relaxed); });
        QObject::connect(
            &window, &QQuickWindow::afterSynchronizing, this,
            [this] {
                syncs_.fetch_add(1, std::memory_order_relaxed);
                synced_at_.store(now_ms(), std::memory_order_relaxed);
            },
            Qt::DirectConnection);
        QObject::connect(&workspace, &Workspace::tilesChanged, this,
                         [this] { retiles_.fetch_add(1, std::memory_order_relaxed); });
    }

    void run() {
        const auto next_agent = press("nextWindow");
        const auto next_category = press("nextCategory");
        for (int index = 0; index < options_.warmup; ++index)
            measure(index % 5 == 4 ? next_category : next_agent, nullptr);
        for (int index = 0; index < options_.switches; ++index) {
            agentKey(next_agent);
            // Every fourth agent switch, also a category switch.
            if (index % 4 == 3) {
                const auto before = workspace_.activeCategoryId();
                measure(next_category, &category_keys_);
                category_keys_.unchanged += workspace_.activeCategoryId() == before ? 1 : 0;
            }
        }
        for (int index = 0; index < options_.switches / 2; ++index) {
            clickCard(index);
            if (index % 6 == 5)
                measure(next_category, nullptr);
        }
        pointerMoves();
    }

    [[nodiscard]] QJsonObject result() const {
        return {{"agentKey", report(agent_keys_)},       {"agentKeyUntiled", report(untiled_)},
                {"agentKeyTiled", report(tiled_)},       {"agentKeyRetiled", report(retiled_)},
                {"categoryKey", report(category_keys_)}, {"agentClick", report(clicks_)},
                {"pointerMove", report(pointer_)}};
    }
    [[nodiscard]] int unchangedKeys() const {
        return agent_keys_.unchanged + category_keys_.unchanged;
    }
    [[nodiscard]] int frameTimeouts() const {
        return agent_keys_.timeouts + category_keys_.timeouts + clicks_.timeouts;
    }

  private:
    // Delivered as the platform plugin delivers input, synchronously: Qt's
    // shortcut map first, then the key event.
    bool key(const QString& binding, QEvent::Type type) {
        const auto combination = QKeySequence(binding)[0];
        const auto when = timestamp_++;
        if (type == QEvent::KeyPress && QWindowSystemInterface::handleShortcutEvent(
                                            &window_, when, combination.key(),
                                            combination.keyboardModifiers(), 0, 0, 0, QString()))
            return true;
        QWindowSystemInterface::handleKeyEvent<QWindowSystemInterface::SynchronousDelivery>(
            &window_, when, type, combination.key(), combination.keyboardModifiers());
        return false;
    }
    std::function<void()> press(const char* action) {
        const auto binding = keymap_.sequences(QString::fromLatin1(action)).value(0);
        require(!binding.isEmpty(), std::string("binding for ") + action);
        return [this, binding] {
            if (key(binding, QEvent::KeyPress))
                return;
            key(binding, QEvent::KeyRelease);
        };
    }
    void mouse(QPointF at, Qt::MouseButtons buttons, Qt::MouseButton button, QEvent::Type type) {
        QWindowSystemInterface::handleMouseEvent<QWindowSystemInterface::SynchronousDelivery>(
            &window_, timestamp_++, at, window_.mapToGlobal(at), buttons, button, type);
    }

    // Returns whether a frame completed. A timeout is counted and the run continues.
    bool measure(const std::function<void()>& act, Stats* into, bool may_draw_nothing = false) {
        const int retiles = retiles_.load(std::memory_order_relaxed);
        const auto* was = workspace_.focusedSession();
        const int swaps = swaps_.load(std::memory_order_relaxed);
        const int syncs = syncs_.load(std::memory_order_relaxed);
        const double start = now_ms();
        act();
        const double dispatched = now_ms();
        if (!wait_for([this, swaps] { return swaps_.load(std::memory_order_relaxed) > swaps; },
                      5000)) {
            if (may_draw_nothing && workspace_.focusedSession() == was)
                return false;
            if (into != nullptr)
                ++into->timeouts;
            pump(options_.gap);
            return false;
        }
        const double framed = now_ms();
        retiled_last_ = retiles_.load(std::memory_order_relaxed) > retiles;
        if (options_.trace)
            trace(was);
        if (into != nullptr)
            into->add(dispatched - start,
                      syncs_.load(std::memory_order_relaxed) > syncs
                          ? synced_at_.load(std::memory_order_relaxed) - start
                          : framed - start,
                      framed - start);
        pump(options_.gap);
        return true;
    }
    void trace(const SessionPreview* was) const {
        std::cerr << "switch " << title(was).toStdString() << " -> "
                  << title(workspace_.focusedSession()).toStdString() << " tiles:";
        for (const auto& value : workspace_.stageTiles())
            std::cerr
                << ' '
                << title(value.toMap().value(QStringLiteral("session")).value<SessionPreview*>())
                       .toStdString();
        std::cerr << '\n';
    }

    void agentKey(const std::function<void()>& next_agent) {
        const auto* before = workspace_.focusedSession();
        const bool tiled = !workspace_.stageTiles().isEmpty();
        const bool sampled = measure(next_agent, &agent_keys_);
        agent_keys_.unchanged += workspace_.focusedSession() == before ? 1 : 0;
        if (!sampled)
            return;
        auto& group = retiled_last_ ? retiled_ : tiled ? tiled_ : untiled_;
        group.add(agent_keys_.dispatch.back(), agent_keys_.synced.back(), agent_keys_.frame.back());
    }

    [[nodiscard]] bool cardOnScreen(const SessionPreview* item) const {
        auto* card =
            find_visual(window_.contentItem(), QStringLiteral("cardPress_") + item->sessionId());
        if (card == nullptr)
            return false;
        const auto box = card->mapRectToScene(card->boundingRect());
        return box.width() > 4 && box.left() >= 0 && box.right() <= window_.width() &&
               box.top() >= 0 && box.bottom() <= window_.height();
    }
    // A click on an on-screen strip card of the active category, cycling.
    void clickCard(int index) {
        const auto list = workspace_.categorySessions();
        const SessionPreview* target = nullptr;
        until(
            [&] {
                target = nullptr;
                for (qsizetype step = 0; step < list.size() && target == nullptr; ++step) {
                    const auto* item =
                        list.at((index * 3 + 1 + step) % list.size()).value<SessionPreview*>();
                    if (item != workspace_.focusedSession() && cardOnScreen(item))
                        target = item;
                }
                return target != nullptr;
            },
            2000, "an on-screen strip card to click");
        const auto name = QStringLiteral("cardPress_") + target->sessionId();
        const auto* before = workspace_.focusedSession();
        const bool sampled = measure(
            [this, &name] {
                auto* item = find_visual(window_.contentItem(), name);
                require(item != nullptr, "strip card " + name.toStdString());
                const auto at = item->mapToScene(QPointF(item->width() / 2, item->height() / 2));
                // Apart in event time, so no two clicks read as a double click.
                timestamp_ += 1000;
                mouse(at, Qt::NoButton, Qt::NoButton, QEvent::MouseMove);
                mouse(at, Qt::LeftButton, Qt::LeftButton, QEvent::MouseButtonPress);
                mouse(at, Qt::NoButton, Qt::LeftButton, QEvent::MouseButtonRelease);
            },
            &clicks_, true);
        if (workspace_.focusedSession() == before) {
            // A click that selected nothing is not a switch sample.
            ++clicks_.unchanged;
            if (sampled)
                clicks_.dropLast();
        }
    }

    // The pointer crossing the window at 120 Hz: each move's delivery, which
    // an interaction log's sampling adds to.
    void pointerMoves() {
        for (int index = 0; index < 1200; ++index) {
            const QPointF at(40 + (index * 13) % (window_.width() - 80),
                             40 + (index * 7) % (window_.height() - 80));
            const double start = now_ms();
            mouse(at, Qt::NoButton, Qt::NoButton, QEvent::MouseMove);
            const double moved = now_ms() - start;
            pointer_.add(moved, moved, moved);
            pump(8);
        }
    }

    Workspace& workspace_;
    const lapis::desktop::KeyMap& keymap_;
    QQuickWindow& window_;
    Options options_;
    std::atomic<int> swaps_{0};
    std::atomic<int> syncs_{0};
    std::atomic<int> retiles_{0};
    std::atomic<double> synced_at_{0.0};
    bool retiled_last_{};
    ulong timestamp_{1};
    Stats agent_keys_;
    Stats untiled_; // agent keys in a category without tiles
    Stats tiled_;   // agent keys in a tiled category, tiles unchanged
    Stats retiled_; // agent keys that changed the tiles
    Stats category_keys_;
    Stats clicks_;
    Stats pointer_;
};

Options parse(const QCoreApplication& app) {
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOptions({
        {"agents", "Agents in all (default 32).", "n", "32"},
        {"categories", "Categories (default 4).", "n", "4"},
        {"switches",
         "Agent-key samples; category keys every fourth, card clicks half as many, and pointer "
         "moves fixed at 1200 (default 300).",
         "n", "300"},
        {"gap-ms", "Idle time between switches (default 60).", "ms", "60"},
        {"warmup", "Unmeasured switches first (default 40).", "n", "40"},
        {"interaction-log", "Record with the interaction log, as lapis.json can turn on."},
        {"trace", "Print each switch and the stage's tiles to standard error."},
        {"output", "Write the JSON result here as well as to stdout.", "path"},
    });
    parser.process(app);
    const Options options{.agents = parser.value("agents").toInt(),
                          .categories = parser.value("categories").toInt(),
                          .switches = parser.value("switches").toInt(),
                          .gap = parser.value("gap-ms").toInt(),
                          .warmup = parser.value("warmup").toInt(),
                          .log = parser.isSet("interaction-log"),
                          .trace = parser.isSet("trace"),
                          .output = parser.value("output")};
    require(options.categories >= 2 && options.agents >= options.categories * 2,
            "at least two categories and two agents per category");
    require(options.switches > 0 && options.warmup >= 0 && options.gap >= 0,
            "positive switches with nonnegative warmup and gap");
#ifndef LAPIS_BENCH_INTERACTION_LOG
    require(!options.log, "this revision has no interaction log");
#endif
    return options;
}

#ifdef LAPIS_BENCH_INTERACTION_LOG
std::unique_ptr<lapis::desktop::InteractionRecorder> recorder(Workspace& workspace,
                                                              const QString& path) {
    auto recorder = std::make_unique<lapis::desktop::InteractionRecorder>(
        workspace, path,
        lapis::desktop::InteractionRecorder::Hooks{
            .secureInput = &lapis::desktop::platform::secure_input_enabled,
            .monotonicUs = {},
            .wallMs = {}});
    lapis::desktop::InteractionLogSettings settings;
    settings.enabled = true;
    recorder->setSettings(settings);
    return recorder;
}
#endif

QJsonObject measure(const Options& options, const QTemporaryDir& home) {
    lapis::desktop::WorkspaceOptions workspace_options;
    workspace_options.storagePath = home.filePath(QStringLiteral("workspace.json"));
    Workspace workspace(lapis::desktop::WorkspaceMode::live, workspace_options);
    require(workspace.workspaceError().isEmpty(), "workspace loads");
    lapis::desktop::KeyMap keymap;
    keymap.setSourcePathForTesting(home.filePath(QStringLiteral("lapis.json")));
    workspace.setLaunchSize(QSize(160, 48));
    const auto folder = home.filePath(QStringLiteral("work"));
    QDir().mkpath(folder);
    const auto agents = start_agents(workspace, options, folder);
    const auto close_agents = qScopeGuard([&workspace] {
        for (const auto& value : workspace.sessions())
            workspace.closeSession(value.value<SessionPreview*>()->sessionId(), true);
        pump(500);
    });
#ifdef LAPIS_BENCH_INTERACTION_LOG
    const auto log =
        options.log
            ? recorder(workspace, home.filePath(QStringLiteral("runtime/interaction.jsonl")))
            : nullptr;
#endif
    lapis::desktop::UiPreview preview(
        workspace, {.source = QUrl::fromLocalFile(QStringLiteral(LAPIS_QML_SOURCE)),
                    .compact = false,
                    .screen = QString(),
                    .keymap = &keymap});
    require(preview.load(), "load Main.qml");
    auto* window = preview.window();
    window->resize(1600, 1000);
    window->requestActivate();
    until([window] { return window->isActive(); }, 5000, "window active");
#ifdef LAPIS_BENCH_INTERACTION_LOG
    if (log)
        log->watchWindow(window);
#endif
    tile_categories(workspace, agents);
    require(workspace.selectCategory(
                workspace.categories().front().toMap().value(QStringLiteral("id")).toString()),
            "first category");
    pump(1500); // first frames, previews, fonts
    Bench bench(workspace, keymap, *window, options);
    bench.run();
    auto result = bench.result();
    result.insert("schema", "lapis.switch-benchmark/1");
    result.insert("mode", "offscreen/software; GUI-thread delivery, scene-graph sync and next "
                          "frameSwapped, not GPU presentation");
    result.insert("pointerMoveMode", "delivery only; not comparable with keyed or clicked frames");
    result.insert("frameTimeouts", bench.frameTimeouts());
    result.insert("agents", options.agents);
    result.insert("categories", options.categories);
    result.insert("tiledCategories", (options.categories + 1) / 2);
    result.insert("interactionLog", options.log);
    result.insert("gapMs", options.gap);
    result.insert("unchangedKeys", bench.unchangedKeys());
    return result;
}

int run(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QQuickWindow::setSceneGraphBackend("software");
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
    QCoreApplication::setAttribute(Qt::AA_MacDontSwapCtrlAndMeta);
    // Short, so session sockets stay inside the local-socket path limit.
    QTemporaryDir home(QStringLiteral("/tmp/lapis-switch-XXXXXX"));
    require(home.isValid(), "private LAPIS_HOME");
    const auto owner_only = QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner;
    QFile::setPermissions(home.path(), owner_only);
    QDir().mkpath(home.filePath(QStringLiteral("runtime")));
    QFile::setPermissions(home.filePath(QStringLiteral("runtime")), owner_only);
    QDir().mkpath(home.filePath(QStringLiteral("bin")));
    write_agent(home.filePath(QStringLiteral("bin/opencode")));
    qputenv("LAPIS_HOME", home.path().toUtf8());
    qputenv("PATH", home.filePath(QStringLiteral("bin")).toUtf8() + ':' + qgetenv("PATH"));
    // Any service still running from this private home ends with the run,
    // however it ends.
    const auto end_services = qScopeGuard([&home] {
        const QStringList pattern{QStringLiteral("-f"), home.path()};
        QProcess::execute(QStringLiteral("/usr/bin/pkill"), pattern);
        // Ending services still write their logs; remove the home after them.
        for (int wait = 0;
             wait < 40 && QProcess::execute(QStringLiteral("/usr/bin/pgrep"),
                                            QStringList{QStringLiteral("-q")} + pattern) == 0;
             ++wait)
            QThread::msleep(50);
    });

    QGuiApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    QCoreApplication::setApplicationName(QStringLiteral("lapis-switch-benchmark"));
    QCoreApplication::setOrganizationName(QStringLiteral("lapis"));
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    qmlRegisterUncreatableType<SessionPreview>("Lapis", 1, 0, "SessionPreview",
                                               "Owned by workspace");
    qmlRegisterType<lapis::desktop::TerminalSurface>("Lapis", 1, 0, "TerminalSurface");
    const auto drain = qScopeGuard([] {
        QCoreApplication::sendPostedEvents();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    });
    const auto options = parse(app);
    const auto result = measure(options, home);
    const auto json = QJsonDocument(result).toJson(QJsonDocument::Indented);
    std::cout << json.toStdString();
    if (!options.output.isEmpty()) {
        QFile file(options.output);
        require(file.open(QIODevice::WriteOnly), "write --output");
        file.write(json);
    }
    if (result.value("unchangedKeys").toInt() > 0) {
        std::cerr << "switch_benchmark: " << result.value("unchangedKeys").toInt()
                  << " switches changed nothing\n";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
} // namespace

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "switch_benchmark: " << error.what() << '\n';
    } catch (...) {
        std::cerr << "switch_benchmark: unknown failure\n";
    }
    return EXIT_FAILURE;
}
