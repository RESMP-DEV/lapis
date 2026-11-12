// The factory view in the production window, always offscreen with the
// software renderer: the visual bar's Factory toggle, the orchestrator's
// thread beside the four-stage pipeline board seeded from a judge's
// factory_state.json, the gate strip, drilling into a worker from an agent
// link or a card and back, and the way back to categories. It never shows a
// window, takes focus or touches the person's files.
//
//   lapis_factory_ui_tests --background [--capture DIR]
//
// With --capture it saves factory_mode_overview.png, factory_drilldown_view.png
// and classic_toggle_restored.png (1440x920) in DIR.
#include "app_window.hpp"
#include "config.hpp"
#include "terminal_surface.hpp"
#include "workspace.hpp"

#include <QColor>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QThread>

#include <array>
#include <cmath>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
using namespace lapis::desktop;
namespace wire = lapis::session::wire;
using Activity = lapis::session::attention::Activity;

void require(bool condition, const std::string& message) {
    if (!condition)
        throw std::runtime_error(message);
}
void pump(int milliseconds) {
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < milliseconds) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
}
void until(const std::function<bool()>& condition, const std::string& message,
           int milliseconds = 5000) {
    QElapsedTimer clock;
    clock.start();
    while (!condition() && clock.elapsed() < milliseconds)
        pump(10);
    require(condition(), message);
}
QQuickItem* find(QQuickItem* parent, const QString& name) {
    if (parent->objectName() == name)
        return parent;
    for (auto* child : parent->childItems())
        if (auto* found = find(child, name))
            return found;
    return nullptr;
}
QQuickItem* item(QQuickWindow& window, const QString& name) {
    auto* found = find(window.contentItem(), name);
    require(found != nullptr, "missing item " + name.toStdString());
    return found;
}
// Shown: visible all the way up and with a size.
bool shown(QQuickWindow& window, const QString& name) {
    auto* found = find(window.contentItem(), name);
    return found != nullptr && found->isVisible() && found->width() > 0 && found->height() > 0;
}
void click(QQuickWindow& window, QQuickItem& target) {
    const auto position = target.mapToScene(QPointF(target.width() / 2, target.height() / 2));
    require(position.x() >= 0 && position.y() >= 0 && position.x() <= window.width() &&
                position.y() <= window.height(),
            "a control is outside the window: " + target.objectName().toStdString());
    const auto global = window.mapToGlobal(position);
    QMouseEvent press(QEvent::MouseButtonPress, position, global, Qt::LeftButton, Qt::LeftButton,
                      Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease, position, global, Qt::LeftButton, Qt::NoButton,
                        Qt::NoModifier);
    QCoreApplication::sendEvent(&window, &press);
    QCoreApplication::sendEvent(&window, &release);
    pump(60);
}
void click(QQuickWindow& window, const QString& name) {
    auto* target = item(window, name);
    require(target->isVisible(), name.toStdString() + " is not visible");
    click(window, *target);
}
// The first text item under `root` whose text holds `needle`.
QQuickItem* text_with(QQuickItem* root, const QString& needle) {
    const auto text = root->property("text");
    if (text.isValid() && text.toString().contains(needle) &&
        root->metaObject()->indexOfSignal("linkActivated(QString)") >= 0)
        return root;
    for (auto* child : root->childItems())
        if (auto* found = text_with(child, needle))
            return found;
    return nullptr;
}
void report(Agent& agent, Activity activity) {
    wire::AttentionSnapshot snapshot;
    snapshot.available = true;
    snapshot.connected = true;
    snapshot.ready = true;
    snapshot.activity = activity;
    agent.applyAttention(snapshot);
}
void write(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    require(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "write " + path.toStdString());
    require(file.write(bytes) == bytes.size(), "write all of " + path.toStdString());
}
QJsonObject verdict(const char* stage, bool keep, const char* headline, const char* reason,
                    const char* receipt, qint64 at) {
    return {{"stage", stage},   {"keepNeeded", keep}, {"headline", headline},
            {"reason", reason}, {"receipt", receipt}, {"judgedAtMs", at}};
}
QJsonObject step(int index, const char* type, const char* field, const QString& text) {
    return {{"step_index", index},
            {"source", type == QLatin1String("USER_INPUT") ? "USER_EXPLICIT" : "MODEL"},
            {"type", type},
            {"status", "DONE"},
            {"created_at",
             QDateTime::currentDateTimeUtc().addSecs(-600 + index * 30).toString(Qt::ISODate)},
            {field, text}};
}

// Eight sample workers in three categories, judged by a separate model, and
// an Antigravity orchestrator whose thread cites them.
void seed(Workspace& workspace, const QTemporaryDir& directory) {
    const auto kernels = workspace.createCategory(QStringLiteral("Kernels"));
    const auto product = workspace.createCategory(QStringLiteral("Product"));
    require(!kernels.isEmpty() && !product.isEmpty(), "sample categories");
    require(workspace.addFixtureAgent("carbon", "Gemini 4 Carbon · night shift", "general", "agy",
                                      "orchestrator\r\n") &&
                workspace.addFixtureAgent("fp8", "FP8 GEMM tuning", kernels, "codex",
                                          "\x1b[34msweep\x1b[0m 3/5  best 1.31x\r\n") &&
                workspace.addFixtureAgent("triage", "Issue triage", product, "claude",
                                          "14 stale issues found\r\n"),
            "sample agents");
    const QList<std::array<const char*, 3>> names{{"renderer", "Renderer · glyph cache", "codex"},
                                                  {"agent", "Adapter bridge", "claude"},
                                                  {"service", "Session service fix", "codex"},
                                                  {"checks", "Release checks", "claude"},
                                                  {"notes", "Docs sweep", "agy"},
                                                  {"shell", "Bench harness", "codex"}};
    for (const auto& [id, title, harness] : names) {
        require(workspace.renameAgent(id, title), std::string("rename ") + id);
        workspace.agent(id)->setHarnessId(QString::fromLatin1(harness));
    }
    require(workspace.moveAgent("renderer", kernels) && workspace.moveAgent("shell", kernels) &&
                workspace.moveAgent("checks", product) && workspace.moveAgent("notes", product),
            "sample categories filled");

    // The orchestrator's conversation, as Antigravity saves it.
    QDir(directory.path()).mkpath("carbon");
    const auto thread = QDir(directory.path()).filePath("carbon/transcript_full.jsonl");
    const QString summary = QStringLiteral(
        "Here is where tonight's lanes stand.\n\n"
        "**2 gates need you**\n\n"
        "- [Adapter bridge](lapis://agent/agent) wants approval to run `migrate` on staging.\n"
        "- [Issue triage](lapis://agent/triage) asks whether to close 14 stale issues.\n\n"
        "**In progress**\n\n"
        "- [Renderer](lapis://agent/renderer): batching glyph uploads, 41/46 tests green.\n"
        "- [FP8 GEMM](lapis://agent/fp8): sweep 3 of 5, best 1.31x over baseline.\n\n"
        "**Landed**\n\n"
        "- [Session service fix](lapis://agent/service) merged as `225b30d`, 9/9 checks.\n"
        "- [Release checks](lapis://agent/checks) notarized v0.9.4.\n\n"
        "**Idle**: the judge marks [Docs sweep](lapis://agent/notes) safe to close; keep "
        "[Bench harness](lapis://agent/shell) parked for the profiling rerun.");
    QByteArray lines;
    for (const auto& value :
         {step(0, "USER_INPUT", "content",
               "<USER_REQUEST>\nSummarize tonight's lanes and tell me what needs "
               "me.\n</USER_REQUEST>"),
          step(1, "PLANNER_RESPONSE", "content", summary),
          step(2, "USER_INPUT", "content",
               "<USER_REQUEST>\nWhich ones can I close?\n</USER_REQUEST>"),
          step(3, "PLANNER_RESPONSE", "content",
               "Close [Docs sweep](lapis://agent/notes): its PR merged two hours ago and nothing "
               "depends on it. Keep the others; the board marks each one.")})
        lines += QJsonDocument(value).toJson(QJsonDocument::Compact) + '\n';
    write(thread, lines);
    require(workspace.setFixtureConversation("carbon", thread), "the orchestrator's thread");

    // Live state: two at work, two finished, two quiet, two waiting on you.
    report(*workspace.agent("renderer"), Activity::working);
    report(*workspace.agent("fp8"), Activity::working);
    report(*workspace.agent("service"), Activity::turn_completed);
    report(*workspace.agent("checks"), Activity::turn_completed);
    report(*workspace.agent("notes"), Activity::idle);
    report(*workspace.agent("shell"), Activity::idle);
    require(workspace.requestAttention({"agent", "gate-1", "Approve: run migrate on staging"}) &&
                workspace.requestAttention({"triage", "gate-2", "Close 14 stale issues?"}),
            "sample requests");

    // The judge's verdicts, after that live state.
    pump(20);
    const auto now = QDateTime::currentMSecsSinceEpoch() + 1000;
    const QJsonObject agents{
        {"renderer", verdict("in_progress", true, "Batching glyph uploads · 41/46 tests green",
                             "Actively editing", "", now)},
        {"fp8", verdict("in_progress", true, "Sweep 3/5 · best 1.31x over cuBLAS baseline",
                        "Running", "", now)},
        {"service", verdict("landed", true, "Merged #81 on main with 9/9 checks passing",
                            "Merged 40m ago", "225b30d · 9/9 checks", now)},
        {"checks", verdict("landed", true, "v0.9.4 notarized and stapled", "Release done",
                           "v0.9.4 · notarized", now)},
        {"notes", verdict("idle_cull", false, "Docs sweep merged", "Merged 2h ago, safe to close",
                          "a41c9e2 · docs", now)},
        {"shell", verdict("idle_cull", true, "Profiling baseline captured",
                          "Holds tonight's profiling baseline for the rerun", "", now)},
        {"agent", verdict("needs_you", true, "Migration ready", "Waiting on approval", "", now)}};
    const auto state = QDir(directory.path()).filePath("factory_state.json");
    write(state, QJsonDocument(QJsonObject{{"version", 1},
                                           {"updatedAtMs", now},
                                           {"judgeModel", "claude-haiku-5-5"},
                                           {"agents", agents}})
                     .toJson());
    workspace.setFactoryStatePath(state);
}

int run(const QString& captures) {
    QTemporaryDir directory;
    require(directory.isValid(), "temporary directory");
    Workspace workspace(WorkspaceMode::fixture);
    seed(workspace, directory);
    require(workspace.factorySummary().value("judged").toBool(), "the judge's file loaded");

    const auto config_path = QDir(directory.path()).filePath("lapis.json");
    write(config_path, R"({"version": 1})");
    Config config;
    config.setSourcePathForTesting(config_path);
    require(config.load() && !config.visualFactory(), "Factory starts off");
    require(workspace.selectAgent("renderer"), "a worker on the stage in the classic view");

    AppWindow preview(workspace, {.source = QUrl::fromLocalFile(QStringLiteral(LAPIS_QML_SOURCE)),
                                  .compact = false,
                                  .screen = QString(),
                                  .config = &config});
    require(preview.load(), "the production window loads");
    auto* window = preview.window();
    require(window != nullptr, "the window exists");
    window->resize(1440, 920);
    window->show();
    window->requestActivate();
    pump(200);
    const auto save = [&](const char* name) {
        if (captures.isEmpty())
            return;
        pump(250);
        const auto path = QDir(captures).filePath(QString::fromLatin1(name));
        require(window->grabWindow().save(path), "save " + path.toStdString());
        std::cout << "factory_ui_test: saved " << name << '\n';
    };

    // Classic: categories and the stage agent; no factory chrome.
    require(!shown(*window, "factoryBoard") && !shown(*window, "factoryHeader") &&
                !shown(*window, "backToFactoryBar"),
            "the classic view has no factory chrome");
    require(shown(*window, "visualFactoryMode") && shown(*window, "visualThisAgent") &&
                shown(*window, "visualAllAgents"),
            "the visual bar offers This agent, All agents and Factory");
    const auto classic_stage = item(*window, "focusedPane")->width();

    // The Factory toggle in the visual bar.
    click(*window, "visualFactoryMode");
    until([&] { return window->property("factoryMode").toBool(); }, "Factory turns on");
    require(config.visualFactory(), "visual.factory is saved on");
    until([&] { return shown(*window, "factoryBoard") && shown(*window, "factoryHeader"); },
          "the board and the orchestrator's bar show");
    until([&] { return workspace.focusedAgent() == workspace.agent("carbon"); },
          "the orchestrator is on the stage");
    until([&] { return shown(*window, "visualView"); }, "the orchestrator shows as its thread");
    require(item(*window, "focusedPane")->width() < classic_stage * 0.75,
            "the thread shares the width with the board");
    const auto board_width = item(*window, "factoryBoard")->width();
    require(board_width > 1440 * 0.3 && board_width < 1440 * 0.45,
            "the board takes about 40% of the window");

    // Every stage, from the seeded verdicts and live state.
    const auto in_stage = [&](const char* stage, const char* agent) {
        auto* card = find(item(*window, QStringLiteral("factoryStage_") + stage),
                          QStringLiteral("factoryCard_") + agent);
        return card != nullptr && card->isVisible();
    };
    until(
        [&] {
            return in_stage("needs_you", "agent") && in_stage("needs_you", "triage") &&
                   in_stage("in_progress", "renderer") && in_stage("in_progress", "fp8") &&
                   in_stage("landed", "service") && in_stage("landed", "checks") &&
                   in_stage("idle_cull", "notes") && in_stage("idle_cull", "shell");
        },
        "the eight workers sit in their four stages");
    require(find(window->contentItem(), "factoryCard_carbon") == nullptr,
            "the orchestrator is not on its own board");
    require(item(*window, "factoryBadge_notes")->property("label").toString() == "Safe to close" &&
                item(*window, "factoryBadge_shell")->property("label").toString() == "Keep parked",
            "idle agents carry the judge's badge");
    require(item(*window, "factoryReceipt_service")->property("label").toString() ==
                "225b30d · 9/9 checks",
            "a landed agent shows its receipt");
    require(shown(*window, "factoryOpen_agent") && shown(*window, "factoryDetails_agent") &&
                shown(*window, "factoryInspect_service") &&
                shown(*window, "factoryClose_service") && shown(*window, "factoryClose_notes") &&
                !shown(*window, "factoryClose_renderer"),
            "each stage offers its own actions");
    require(
        item(*window, "factoryJudgeLine")->property("text").toString().contains("claude-haiku-5-5"),
        "the board names the judge");
    until([&] { return shown(*window, "homeAttentionInbox"); },
          "the gate strip shows above the orchestrator's composer");
    require(shown(*window, "homeAttentionChip_agent") && shown(*window, "homeAttentionChip_triage"),
            "the strip lists both requests");
    // At 1440x920 the header reads in full and all eight cards fit.
    require(item(*window, "factoryHomeTitle")->property("text").toString() ==
                    "Gemini 4 Carbon · night shift" &&
                !item(*window, "factoryHomeTitle")->property("truncated").toBool(),
            "the orchestrator's title is not elided: width " +
                std::to_string(item(*window, "factoryHomeTitle")->width()) + " of " +
                std::to_string(item(*window, "factoryHomeTitle")->implicitWidth()) +
                ", header " + std::to_string(item(*window, "factoryHeader")->width()) +
                ", selector " + std::to_string(item(*window, "factoryHomeSelector")->width()) +
                ", summary " + std::to_string(item(*window, "factoryHeaderSummary")->width()) +
                " of " + std::to_string(item(*window, "factoryHeaderSummary")->implicitWidth()) +
                ", pin " + std::to_string(item(*window, "factoryHomePin")->implicitWidth()) +
                ", exit " + std::to_string(item(*window, "exitFactory")->width()));
    require(item(*window, "factoryHeaderSummary")->property("text").toString() ==
                    "2 need you · 2 running · 2 landed · 2 idle" &&
                !item(*window, "factoryHeaderSummary")->property("truncated").toBool(),
            "the header's counts are not elided");
    {
        auto* scroller = item(*window, "factoryScroller");
        for (const char* name : {"factoryBadge_shell", "factoryInspect_shell", "factoryClose_shell"}) {
            auto* control = item(*window, QString::fromLatin1(name));
            const auto bottom =
                control->mapToItem(scroller, QPointF(0, control->height())).y();
            require(control->isVisible() && bottom <= scroller->height(),
                    std::string(name) + " is clipped at the board's bottom");
        }
    }
    save("factory_mode_overview.png");

    // An agent link in the orchestrator's thread drills into that worker.
    auto* view = item(*window, "visualView");
    QQuickItem* reply = nullptr;
    until([&] { return (reply = text_with(view, "lapis://agent/renderer")) != nullptr; },
          "the thread renders the agent links");
    require(QMetaObject::invokeMethod(reply, "linkActivated",
                                      Q_ARG(QString, QStringLiteral("lapis://agent/rend"))),
            "activate the link");
    until([&] { return workspace.focusedAgent() == workspace.agent("renderer"); },
          "the link shows the worker");
    require(window->property("factoryInspectAgentId").toString() == "renderer",
            "the link inspects the worker");
    until([&] { return shown(*window, "backToFactoryBar"); }, "the bar back to the factory shows");
    until([&] { return !shown(*window, "factoryBoard") && !shown(*window, "factoryHeader"); },
          "the board and the orchestrator's bar hide while inspecting");
    click(*window, "backToFactoryBar");
    until([&] { return workspace.focusedAgent() == workspace.agent("carbon"); },
          "Back to Factory returns to the orchestrator");
    require(shown(*window, "factoryBoard") && !shown(*window, "backToFactoryBar") &&
                window->property("factoryInspectAgentId").toString().isEmpty(),
            "the split view is back");

    // A card's Inspect does the same, with the stage and receipt in the bar.
    until([&] { return shown(*window, "factoryInspect_service"); }, "the card's Inspect shows");
    click(*window, "factoryInspect_service");
    until([&] { return workspace.focusedAgent() == workspace.agent("service"); },
          "Inspect shows the worker");
    require(shown(*window, "backToFactoryBar") && shown(*window, "backToFactoryStage") &&
                item(*window, "backToFactoryStage")->property("stageId").toString() == "landed" &&
                item(*window, "backToFactoryStage")->property("accent").value<QColor>() ==
                    window->property("plentyColor").value<QColor>(),
            "the bar carries the worker's stage");
    save("factory_drilldown_view.png");
    click(*window, "backToFactoryBar");
    until([&] { return shown(*window, "factoryBoard"); }, "back from the card");

    // Choosing the orchestrator elsewhere, or an agent elsewhere, follows.
    require(workspace.selectAgent("checks"), "choose a worker from the sidebar");
    until([&] { return shown(*window, "backToFactoryBar"); }, "the sidebar inspects too");
    require(workspace.selectAgent("carbon"), "choose the orchestrator");
    until([&] { return shown(*window, "factoryBoard"); }, "choosing the orchestrator goes back");

    // The board collapses and expands in one click.
    click(*window, "factoryBoardCollapse");
    until([&] { return item(*window, "factoryBoard")->width() < 80; }, "the board collapses");
    require(item(*window, "focusedPane")->width() > classic_stage * 0.8,
            "the thread takes the room");
    click(*window, "factoryBoardExpand");
    until([&] { return item(*window, "factoryBoard")->width() > 300; }, "the board expands");

    // A request answered leaves the strip.
    require(workspace.resolveAttention({"triage", "gate-2", {}}), "resolve a request");
    until([&] { return find(window->contentItem(), "homeAttentionChip_triage") == nullptr; },
          "the strip drops the resolved request");

    // The command palette lists the toggle.
    QVariant entries;
    require(QMetaObject::invokeMethod(window, "commandEntries", Q_RETURN_ARG(QVariant, entries)),
            "read the palette");
    bool listed = false;
    for (const auto& value : entries.toList()) {
        const auto entry = value.toMap();
        listed = listed || (entry.value("id").toString() == "toggleFactory" &&
                            entry.value("label").toString() == "Toggle Factory view" &&
                            entry.value("enabled").toBool());
    }
    require(listed, "the palette offers Toggle Factory view");

    // Classic view from the factory bar restores the categories.
    click(*window, "exitFactory");
    until([&] { return !window->property("factoryMode").toBool(); }, "Classic view turns it off");
    require(!config.visualFactory(), "visual.factory is saved off");
    until([&] { return !shown(*window, "factoryBoard") && !shown(*window, "factoryHeader"); },
          "the factory chrome goes");
    require(workspace.selectAgent("renderer"), "a worker on the classic stage");
    until(
        [&] {
            return std::abs(item(*window, "focusedPane")->width() - classic_stage) < 2 &&
                   !shown(*window, "visualView");
        },
        "the classic stage fills its width with the terminal");
    save("classic_toggle_restored.png");

    // The shortcut and the palette command toggle it too.
    require(QMetaObject::invokeMethod(window, "runCommand",
                                      Q_ARG(QVariant, QStringLiteral("toggleFactory"))),
            "run the palette command");
    until([&] { return window->property("factoryMode").toBool(); }, "the palette turns it on");
    if (window->isActive()) {
        QKeyEvent press(QEvent::KeyPress, Qt::Key_F, Qt::MetaModifier | Qt::ShiftModifier);
        QKeyEvent release(QEvent::KeyRelease, Qt::Key_F, Qt::MetaModifier | Qt::ShiftModifier);
        QCoreApplication::sendEvent(window, &press);
        QCoreApplication::sendEvent(window, &release);
        until([&] { return !window->property("factoryMode").toBool(); },
              "Command-Shift-F turns it off");
    } else {
        std::cout << "factory_ui_test: window not active offscreen; shortcut not exercised\n";
        require(QMetaObject::invokeMethod(window, "toggleFactory"), "toggle back");
    }

    // Both themes render the board without QML errors.
    require(QMetaObject::invokeMethod(window, "toggleFactory"), "the factory again");
    until([&] { return shown(*window, "factoryBoard"); }, "the board for the theme pass");
    require(config.setAppearance(QStringLiteral("light")), "light appearance");
    pump(150);
    require(preview.diagnostics().isEmpty(),
            "QML diagnostics in light: " + preview.diagnostics().toStdString());
    require(config.setAppearance(QStringLiteral("dark")), "dark appearance");
    pump(150);

    // Close from a card: Safe to close goes in one click; a parked agent asks.
    auto* close_dialog = window->findChild<QObject*>(QStringLiteral("closeAgentDialog"));
    require(close_dialog != nullptr, "the close dialog exists");
    until([&] { return shown(*window, "factoryClose_shell"); }, "the parked card's Close");
    click(*window, "factoryClose_shell");
    until([&] { return close_dialog->property("visible").toBool(); },
          "Close on a parked agent asks first");
    require(workspace.agent("shell") != nullptr &&
                close_dialog->property("agentId").toString() == "shell" &&
                close_dialog->property("agentName").toString() == "Codex",
            "the dialog names the harness and nothing closed yet");
    require(QMetaObject::invokeMethod(close_dialog, "close"), "cancel");
    until([&] { return !close_dialog->property("visible").toBool(); }, "the dialog closes");
    pump(100);
    require(workspace.agent("shell") != nullptr, "cancelling keeps the agent");
    click(*window, "factoryClose_shell");
    until([&] { return close_dialog->property("visible").toBool(); }, "asks again");
    require(QMetaObject::invokeMethod(close_dialog, "confirm"), "confirm");
    until([&] { return workspace.agent("shell") == nullptr; }, "confirming closes the agent");
    until([&] { return shown(*window, "factoryClose_notes"); }, "the safe card's Close");
    click(*window, "factoryClose_notes");
    until([&] { return workspace.agent("notes") == nullptr; }, "Safe to close closes at once");
    require(!close_dialog->property("visible").toBool(), "and asks nothing");

    require(preview.diagnostics().isEmpty(),
            "QML diagnostics: " + preview.diagnostics().toStdString());
    std::cout << "factory_ui_test: PASS (offscreen/software; not native input or GPU acceptance)\n";
    return EXIT_SUCCESS;
}

} // namespace

int main(int argc, char** argv) {
    QString captures;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--background")
            continue; // always: this test never shows a window
        if (argument == "--capture" && index + 1 < argc) {
            captures = QString::fromLocal8Bit(argv[++index]);
            continue;
        }
        std::cerr << "Usage: lapis_factory_ui_tests [--background] [--capture DIR]\n";
        return EXIT_FAILURE;
    }
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QQuickWindow::setSceneGraphBackend("software");
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
    QCoreApplication::setAttribute(Qt::AA_MacDontSwapCtrlAndMeta);
    QGuiApplication application(argc, argv);
    application.setQuitOnLastWindowClosed(false);
    QCoreApplication::setApplicationName(QStringLiteral("lapis-factory-ui-tests"));
    QCoreApplication::setOrganizationName(QStringLiteral("lapis"));
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    qmlRegisterUncreatableType<Agent>("Lapis", 1, 0, "Agent", "Owned by workspace");
    qmlRegisterType<TerminalSurface>("Lapis", 1, 0, "TerminalSurface");
    const auto drain = qScopeGuard([] {
        QCoreApplication::sendPostedEvents();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    });
    try {
        require(QGuiApplication::platformName() == QStringLiteral("offscreen"),
                "offscreen platform only");
        return run(captures);
    } catch (const std::exception& error) {
        std::cerr << "factory_ui_test: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
