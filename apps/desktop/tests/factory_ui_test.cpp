// The factory view in the production window, always offscreen with the
// software renderer: the visual bar's Factory toggle, the orchestrator's
// thread beside the four-stage pipeline board seeded from a judge's
// factory_state.json, the gate strip, drilling into a worker from an agent
// link or a card and back, and the way back to categories. It never shows a
// window, takes focus or touches the person's files.
//
//   lapis_factory_ui_tests --background [--new-session | --tab-flow] [--capture DIR]
//
// With --capture it saves factory_mode_overview.png, factory_drilldown_view.png
// and classic_toggle_restored.png (1440x920) in DIR. --new-session checks a
// new session opening on its thread instead (run_new_session), and saves
// new_session_empty.png, new_session_first_send.png and
// new_session_trust_screen.png (1200x800). --tab-flow checks Tab, Tab, Tab
// in the thread's bar across agents (tab_flow::run_tab_flow), and saves
// tab_flow_offer.png and tab_flow_nothing_waiting.png (1200x800).
#include "app_window.hpp"
#include "config.hpp"
#include "terminal_surface.hpp"
#include "transcript.hpp"
#include "workspace.hpp"

#include <QColor>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHash>
#include <QInputMethodEvent>
#include <QJSEngine>
#include <QJSValue>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QQmlEngine>
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
                std::to_string(item(*window, "factoryHomeTitle")->implicitWidth()) + ", header " +
                std::to_string(item(*window, "factoryHeader")->width()) + ", selector " +
                std::to_string(item(*window, "factoryHomeSelector")->width()) + ", summary " +
                std::to_string(item(*window, "factoryHeaderSummary")->width()) + " of " +
                std::to_string(item(*window, "factoryHeaderSummary")->implicitWidth()) + ", pin " +
                std::to_string(item(*window, "factoryHomePin")->implicitWidth()) + ", exit " +
                std::to_string(item(*window, "exitFactory")->width()));
    require(item(*window, "factoryHeaderSummary")->property("text").toString() ==
                    "2 need you · 2 running · 2 landed · 2 idle" &&
                !item(*window, "factoryHeaderSummary")->property("truncated").toBool(),
            "the header's counts are not elided");
    {
        auto* scroller = item(*window, "factoryScroller");
        for (const char* name :
             {"factoryBadge_shell", "factoryInspect_shell", "factoryClose_shell"}) {
            auto* control = item(*window, QString::fromLatin1(name));
            const auto bottom = control->mapToItem(scroller, QPointF(0, control->height())).y();
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
    // A sample card (no CLI behind it) keeps its terminal.
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

// A Claude Code screen as a new session draws it, made up in its shape.
QString claude_screen(const QString& body) {
    return QStringLiteral(" * Welcome to Claude Code\r\n\r\n") + body;
}
QString claude_ready() {
    const QString rule(60, QChar(0x2500));
    return claude_screen(rule + QStringLiteral("\r\n❯ \r\n") + rule +
                         QStringLiteral("\r\n  ? for shortcuts\r\n"));
}
// The same, with a welcome line naming its model as Claude Code's box does.
QString claude_welcome(const QString& line) {
    const QString rule(60, QChar(0x2500));
    return claude_screen(QStringLiteral(" ") + line + QStringLiteral("\r\n\r\n") + rule +
                         QStringLiteral("\r\n❯ \r\n") + rule +
                         QStringLiteral("\r\n  ? for shortcuts\r\n"));
}
QString claude_trust() {
    const QString rule(60, QChar(0x2500));
    return rule + QStringLiteral("\r\n Accessing workspace:\r\n\r\n /work/project\r\n\r\n"
                                 " Is this a project you created or one you trust?\r\n\r\n"
                                 " ❯ 1. Yes, I trust this folder\r\n   2. No, exit\r\n\r\n"
                                 " Enter to confirm · Esc to cancel\r\n");
}
void show_screen(Agent& agent, const QString& content) {
    lapis::session::Terminal terminal({100, 30});
    terminal.feed(content.toStdString());
    agent.applySnapshot(terminal.snapshot());
}

// A new session opens on its thread, never its terminal: an empty thread
// before its CLI saved anything (no other agent's rows), the bar holding
// what is typed while the CLI starts, a first prompt shown at once and the
// file taking over when it appears, the terminal shown for the CLI's own
// startup screen and covered again when it closes, with visual.all on by
// default. Offscreen and software; fixture screens, never a real CLI.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
int run_new_session(const QString& captures) {
    QTemporaryDir directory;
    require(directory.isValid(), "temporary directory");
    Workspace workspace(WorkspaceMode::fixture);
    require(
        workspace.addFixtureAgent("older", "Older thread", "general", "claude", claude_trust()) &&
            workspace.addFixtureAgent("fresh", "New Claude Code", "general", "claude",
                                      claude_ready()) &&
            workspace.addFixtureAgent("booting", "New Codex", "general", "codex", "") &&
            workspace.addFixtureAgent("trust", "Untrusted folder", "general", "claude",
                                      claude_trust()) &&
            workspace.addFixtureAgent("plain", "Kept terminal", "general", "claude",
                                      claude_ready()) &&
            workspace.addFixtureAgent("defaulted", "No model given", "general", "claude",
                                      claude_ready()) &&
            workspace.addFixtureAgent("welcome", "Welcome names it", "general", "claude",
                                      claude_welcome(QStringLiteral("Sonnet 5.5 · Claude Max"))) &&
            workspace.addFixtureAgent("remote", "On another machine", "general", "claude",
                                      claude_ready()),
        "sample agents");
    for (const char* id :
         {"older", "fresh", "booting", "trust", "plain", "defaulted", "welcome", "remote"})
        report(*workspace.agent(id), Activity::idle);
    // What each was started with, as a saved agent keeps it (never run): a
    // model and level, Codex's -m and -c, none, and a remote command there.
    require(workspace.setFixtureLaunch("fresh", QStringLiteral("claude"),
                                       {QStringLiteral("--model"), QStringLiteral("opus"),
                                        QStringLiteral("--effort"), QStringLiteral("xhigh")}) &&
                workspace.setFixtureLaunch(
                    "booting", QStringLiteral("codex"),
                    {QStringLiteral("-m"), QStringLiteral("gpt-6-sol"), QStringLiteral("-c"),
                     QStringLiteral("model_reasoning_effort=high")}) &&
                workspace.setFixtureLaunch(
                    "remote", QStringLiteral("/usr/bin/ssh"),
                    {QStringLiteral("-o"), QStringLiteral("ControlPath=none"),
                     QStringLiteral("-t"), QStringLiteral("box"),
                     QStringLiteral(
                         R"(cd '/work/x' && s=0199a000-0000-7000-8000-00000000c0de && )"
                         R"(o=--session-id && { exec "${SHELL:-/bin/sh}" -lic )"
                         R"('claude --model opus --effort high '"$o $s"; })")}),
            "launch commands");
    // Claude Code's model list from a stand-in CLI: its default entry names
    // Fable 5.2, and "opus" is an alias for claude-opus-5-5.
    const auto stand_in = QDir(directory.path()).filePath("bin/claude");
    QDir().mkpath(QFileInfo(stand_in).absolutePath());
    write(stand_in,
          "#!/bin/sh\nread -r line\n"
          R"sh(echo '{"type":"control_response","response":{"subtype":"success","request_id":"models","response":{"models":[)sh"
          R"sh({"value":"default","displayName":"Default (recommended)","description":"Fable 5.2 · Best for everyday tasks","resolvedModel":"claude-fable-5-2","supportedEffortLevels":["low","medium","high"]},)sh"
          R"sh({"value":"opus","displayName":"Opus 5.5","resolvedModel":"claude-opus-5-5","supportedEffortLevels":["low","high","xhigh","max"]}]}}}')sh"
          "\ncat >/dev/null\n");
    QFile::setPermissions(stand_in, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                        QFileDevice::ExeOwner);
    HarnessModels models(
        [&](const QString& id) { return id == QLatin1String("claude") ? stand_in : QString(); },
        QDir(directory.path()).filePath("home"));
    models.refresh();
    until([&] { return !models.models(QStringLiteral("claude")).empty(); },
          "the stand-in's model list");
    workspace.setHarnessModels(&models);
    const auto older = QDir(directory.path()).filePath("older.jsonl");
    write(
        older,
        R"({"type":"user","uuid":"o-1","message":{"content":"An older question"}})"
        "\n"
        R"({"type":"assistant","uuid":"o-2","message":{"content":[{"type":"text","text":"An older answer."}]}})"
        "\n");
    require(workspace.setFixtureConversation("older", older), "the older thread");

    const auto config_path = QDir(directory.path()).filePath("lapis.json");
    write(config_path, R"({"version": 1})");
    Config config;
    config.setSourcePathForTesting(config_path);
    require(config.load() && config.visualAll(), "visual.all is on for a config without it");
    require(workspace.selectAgent("older"), "an agent with a saved thread first");

    AppWindow preview(workspace, {.source = QUrl::fromLocalFile(QStringLiteral(LAPIS_QML_SOURCE)),
                                  .compact = false,
                                  .screen = QString(),
                                  .config = &config});
    require(preview.load(), "the production window loads");
    auto* window = preview.window();
    require(window != nullptr, "the window exists");
    window->resize(1200, 800);
    window->show();
    window->requestActivate();
    pump(200);
    auto* model = qobject_cast<TranscriptModel*>(workspace.transcript());
    require(model != nullptr, "the thread's model");
    const auto flag = [&](const char* name) { return window->property(name).toBool(); };
    const auto text_of = [&](const char* name) {
        return item(*window, QString::fromLatin1(name))->property("text").toString();
    };

    // An agent with a file shows its thread, as before. Its screen shows a
    // dialog, which never uncovers it: only the person's own command does.
    until([&] { return shown(*window, "visualView") && model->rowCount() > 0; },
          "the older agent shows its thread");
    pump(1500);
    require(flag("visualCovers") && !flag("visualStartScreen"),
            "a dialog on an agent with a file does not uncover its terminal");

    // A new session: covered at once by an empty thread, no older rows.
    require(workspace.selectAgent("fresh"), "the new Claude Code session");
    until([&] { return flag("visualCovers") && shown(*window, "visualBlankState"); },
          "the new session opens on its empty thread");
    require(model->rowCount() == 0 && model->path().isEmpty() &&
                model->formatName() == QStringLiteral("claude"),
            "the empty thread shows nothing of the older agent's");
    require(text_of("visualBlankTitle") == QStringLiteral("Claude Code"), "it names the CLI");
    until(
        [&] {
            return text_of("visualBlankStatus") == QStringLiteral("Ready for your first message");
        },
        "its input box shows, so it is ready");
    require(!flag("visualStarting"), "the bar sends");
    // The bar names the model and level it was started with, from the
    // start: the menu's names, its tick and that model's levels, not pending.
    const auto label_of = [&](const char* name) {
        return item(*window, QString::fromLatin1(name))->property("label").toString();
    };
    const auto bar = [&] { return label_of("visualModel") + " / " + label_of("visualEffort"); };
    auto* thread_view = item(*window, "visualView");
    const auto ticked = [&] {
        return thread_view->property("currentEntry").toMap().value("id").toString();
    };
    until([&] { return bar() == "Opus 5.5 / Extra High"; },
          "a new session shows its launch model and level: " + bar().toStdString());
    require(ticked() == "opus" &&
                thread_view->property("currentEfforts").toStringList() ==
                    QStringList({"low", "high", "xhigh", "max"}) &&
                !model->choicePending(),
            "the menu ticks it and offers its levels before any reply");
    QVariant entries;
    require(QMetaObject::invokeMethod(window, "commandEntries", Q_RETURN_ARG(QVariant, entries)),
            "read the palette");
    bool offered = false;
    for (const auto& value : entries.toList()) {
        const auto entry = value.toMap();
        offered = offered || (entry.value("id").toString() == "toggleVisual" &&
                              entry.value("label").toString() == "Show terminal" &&
                              entry.value("enabled").toBool());
    }
    require(offered, "Commands offers its terminal");
    const auto save = [&](const char* name) {
        if (captures.isEmpty())
            return;
        pump(250);
        const auto path = QDir(captures).filePath(QString::fromLatin1(name));
        require(window->grabWindow().save(path), "save " + path.toStdString());
        std::cout << "factory_ui_test: saved " << name << '\n';
    };
    save("new_session_empty.png");

    // The first prompt goes through the bar's own send (the fixture has no
    // live terminal, so a stand-in takes what would be typed into it),
    // shows at once, the file is looked for twice a second, and the thread
    // takes over when the CLI saves it.
    auto* view = item(*window, "visualView");
    auto* input = qvariant_cast<QQuickItem*>(view->property("composer"));
    require(input != nullptr, "the bar's text field");
    auto* engine = qmlEngine(view);
    require(engine != nullptr, "the window's engine");
    auto typed = engine->evaluate(QStringLiteral(
        "(function() { var sent = []; var f = function(text) { sent.push(text); return true };"
        " f.sent = sent; return f })()"));
    require(typed.isCallable(), "a stand-in terminal");
    view->setProperty("submit", QVariant::fromValue(typed));
    input->setProperty("text", QStringLiteral("Summarize the README"));
    require(QMetaObject::invokeMethod(view, "send"), "press Send");
    require(typed.property("sent").property(0).toString() == QStringLiteral("Summarize the README"),
            "the prompt goes to the terminal: " +
                view->property("notice").toString().toStdString());
    require(input->property("text").toString().isEmpty(), "the bar clears");
    until([&] { return flag("visualFirstSend") && shown(*window, "pendingPrompt"); },
          "the first prompt shows as pending");
    require(!shown(*window, "visualBlankState"), "the empty state gives way to it");
    require(bar() == "Opus 5.5 / Extra High", "through the first send: " + bar().toStdString());
    save("new_session_first_send.png");
    const auto fresh = QDir(directory.path()).filePath("fresh.jsonl");
    write(fresh, R"({"type":"user","uuid":"f-1","message":{"content":"Summarize the README"}})"
                 "\n");
    require(workspace.setFixtureConversation("fresh", fresh), "the CLI saves its first record");
    QElapsedTimer takeover;
    takeover.start();
    until([&] { return model->path() == fresh && model->rowCount() > 0; },
          "the thread takes over from its file");
    require(takeover.elapsed() < 1500,
            "the file is found within a second: " + std::to_string(takeover.elapsed()) + " ms");
    require(model->pendingPrompts().isEmpty() && !flag("visualFirstSend") &&
                !shown(*window, "visualBlankState") && flag("visualCovers"),
            "the recorded prompt leaves pending and the thread stays covering");
    require(model->model().isEmpty() && bar() == "Opus 5.5 / Extra High",
            "the file holds no reply yet: still what it was started with");
    // The reply reports the alias's API id: no change in the bar.
    {
        QFile file(fresh);
        require(file.open(QIODevice::Append), "append the reply");
        file.write(
            R"({"type":"assistant","uuid":"f-2","effort":"xhigh","message":{"model":"claude-opus-5-5","content":[{"type":"text","text":"It is a README."}]}})"
            "\n");
    }
    until([&] { return model->model() == "claude-opus-5-5"; }, "the reply is read");
    require(bar() == "Opus 5.5 / Extra High" && ticked() == "opus",
            "the reported API id keeps the same name and tick: " + bar().toStdString());

    // A CLI still starting: nothing drawn, the bar holds what is typed.
    require(workspace.selectAgent("booting"), "the new Codex session");
    until([&] { return flag("visualCovers") && flag("visualStarting"); },
          "a starting CLI still opens on its thread");
    require(model->rowCount() == 0 && model->formatName() == QStringLiteral("codex"),
            "an empty Codex thread");
    require(text_of("visualBlankTitle") == QStringLiteral("Codex") &&
                text_of("visualBlankStatus") == QStringLiteral("Starting…"),
            "it says the CLI is starting");
    input->setProperty("text", QStringLiteral("Run the sweep"));
    require(QMetaObject::invokeMethod(view, "send"), "press Send");
    require(input->property("text").toString() == QStringLiteral("Run the sweep") &&
                model->pendingPrompts().isEmpty() &&
                view->property("notice").toString().contains(QStringLiteral("still starting")),
            "nothing goes to a CLI that is not ready; the message stays in the bar");
    input->setProperty("text", QString());
    require(bar() == "gpt-6-sol / High" && shown(*window, "visualModel"),
            "Codex: its -m and level, never the agent shown before: " + bar().toStdString());

    // Started with no model: Claude Code's default entry names it, and the
    // menu ticks that entry.
    require(workspace.selectAgent("defaulted"), "a Claude Code started with no model");
    until([&] { return bar() == "Fable 5.2 / Effort"; },
          "its CLI's listed default: " + bar().toStdString());
    require(ticked() == "default" && !model->choicePending() &&
                thread_view->property("currentEfforts").toStringList() ==
                    QStringList({"low", "medium", "high"}),
            "the default entry is ticked with its levels");
    // A pick before its file exists shows pending and stays with the agent.
    model->choose(QStringLiteral("opus"), QStringLiteral("low"), {}, {});
    until([&] { return bar() == "Opus 5.5… / Low…"; },
          "a pick before any file shows at once: " + bar().toStdString());
    // Its start screen names a model: that, over the listed default.
    require(workspace.selectAgent("welcome"), "a Claude Code whose welcome names its model");
    until([&] { return bar() == "Sonnet 5.5 / Effort"; },
          "what its screen says: " + bar().toStdString());
    require(workspace.selectAgent("defaulted"), "back");
    until([&] { return bar() == "Opus 5.5… / Low…"; },
          "the pick is still that agent's: " + bar().toStdString());
    const auto defaulted = QDir(directory.path()).filePath("defaulted.jsonl");
    write(defaulted, R"({"type":"user","uuid":"d-1","message":{"content":"Go"}})"
                     "\n");
    require(workspace.setFixtureConversation("defaulted", defaulted), "its CLI saves a file");
    until([&] { return model->path() == defaulted; }, "the file shows");
    require(bar() == "Opus 5.5… / Low…", "the pick carries into the file: " + bar().toStdString());

    // A remote agent: the words of its command there.
    const auto remote = QDir(directory.path()).filePath("remote.jsonl");
    write(remote, R"({"type":"user","uuid":"r-1","message":{"content":"Go"}})"
                  "\n");
    require(workspace.setFixtureConversation("remote", remote), "the remote agent's mirror");
    require(workspace.selectAgent("remote"), "the remote agent");
    until([&] { return model->path() == remote && bar() == "Opus 5.5 / High"; },
          "a remote agent's launch model and level: " + bar().toStdString());

    // A trust prompt in place of the input box: the terminal shows, with a
    // note, and the thread covers again once the input box is back.
    require(workspace.selectAgent("trust"), "a new session on a trust prompt");
    until([&] { return flag("visualStartScreen") && !flag("visualCovers"); },
          "the CLI's own startup screen uncovers the terminal");
    require(shown(*window, "liveTerminal") && !shown(*window, "visualView") &&
                shown(*window, "visualDialogNote"),
            "the terminal shows with a note why");
    require(model->pendingPrompts().isEmpty(), "lapis typed nothing into it");
    save("new_session_trust_screen.png");
    show_screen(*workspace.agent("trust"), claude_ready());
    until([&] { return flag("visualCovers") && !flag("visualStartScreen"); },
          "answered, the thread covers the terminal again");
    require(shown(*window, "visualBlankState") && !shown(*window, "visualDialogNote"),
            "the empty thread is back");

    // A person's own "Show terminal" still wins, and a sample shell (no
    // CLI the view reads) shows its terminal.
    require(workspace.selectAgent("plain"), "another new session");
    until([&] { return flag("visualCovers"); }, "on its thread");
    require(QMetaObject::invokeMethod(window, "toggleVisual"), "Show terminal");
    until([&] { return !flag("visualActive") && !shown(*window, "visualView"); },
          "the person's choice shows the terminal");
    require(workspace.selectAgent("shell"), "a sample agent no reader knows");
    pump(300);
    require(!flag("visualActive") && !shown(*window, "visualView"),
            "an agent with no CLI the view reads shows its terminal");
    require(workspace.selectAgent("plain"), "back to the kept terminal");
    pump(300);
    require(!flag("visualActive"), "the choice holds for this run");

    require(preview.diagnostics().isEmpty(),
            "QML diagnostics: " + preview.diagnostics().toStdString());
    std::cout << "factory_ui_test: new sessions PASS (offscreen/software fixture screens; not a "
                 "real CLI's startup, native input or GPU acceptance)\n";
    return EXIT_SUCCESS;
}

} // namespace

namespace tab_flow {
// A stand-in for NextPrompt with its QML surface and its bookkeeping: an
// offer goes once it is used (Tab typed it), a "sent" use needs that earlier
// use of the same offer, and an impression counts once per offer. It never
// runs the helper or calls a model; it records what the window reports.
class Guesses final : public QObject {
    Q_OBJECT
    Q_PROPERTY(int revision READ revision NOTIFY changed)
    Q_PROPERTY(bool enabled READ enabled NOTIFY changed)
    Q_PROPERTY(double confidentThreshold READ confidentThreshold CONSTANT)
    Q_PROPERTY(int failureStreak READ failureStreak CONSTANT)
    Q_PROPERTY(QString lastFailure READ lastFailure CONSTANT)
  public:
    struct Offer {
        QString text;
        QString key;
        bool seen{};
        QString author;
    };
    void offer(const QString& id, const QString& text, const QString& key,
               const QString& author = {}) {
        offers_.insert(id, {text, key, false, author});
        ++revision_;
        emit changed();
    }
    [[nodiscard]] const QStringList& events() const { return events_; }
    [[nodiscard]] int revision() const { return revision_; }
    [[nodiscard]] static bool enabled() { return true; }
    [[nodiscard]] static double confidentThreshold() { return 0.45; }
    [[nodiscard]] static int failureStreak() { return 0; }
    [[nodiscard]] static QString lastFailure() { return {}; }
    Q_INVOKABLE [[nodiscard]] static bool guesses(const QString& cli) {
        return cli == QLatin1String("claude") || cli == QLatin1String("codex") ||
               cli == QLatin1String("omp");
    }
    Q_INVOKABLE [[nodiscard]] QString suggestion(const QString& id) const {
        return offers_.value(id).text;
    }
    Q_INVOKABLE [[nodiscard]] QString author(const QString& id) const {
        return offers_.value(id).author;
    }
    Q_INVOKABLE [[nodiscard]] static double confidence(const QString&) { return 0.5; }
    Q_INVOKABLE [[nodiscard]] QString offerKey(const QString& id) const {
        return offers_.value(id).key;
    }
    Q_INVOKABLE [[nodiscard]] QVariantMap readyAgents() const {
        QVariantMap ready;
        for (auto offer = offers_.cbegin(); offer != offers_.cend(); ++offer)
            ready.insert(offer.key(), offer->seen);
        return ready;
    }
    Q_INVOKABLE void seenOffer(const QVariantMap& identity) {
        const auto id = identity.value(QStringLiteral("session")).toString();
        const auto key = identity.value(QStringLiteral("offer")).toString();
        auto offer = offers_.find(id);
        if (offer == offers_.end() || key.isEmpty() || offer->key != key || offer->seen)
            return;
        offer->seen = true;
        events_.append(QStringLiteral("seen ") + key);
    }
    Q_INVOKABLE void used(const QString& id, bool sent, int, const QString& key = {}) {
        if (sent) {
            if (!filled_.contains(id) || (!key.isEmpty() && filled_.value(id) != key))
                return;
            events_.append(QStringLiteral("sent ") + filled_.value(id));
            return;
        }
        const auto offer = offers_.value(id);
        if (offer.text.isEmpty() || (!key.isEmpty() && offer.key != key))
            return;
        events_.append(QStringLiteral("filled ") + offer.key);
        filled_.insert(id, offer.key);
        offers_.remove(id);
        ++revision_;
        emit changed();
    }
  signals:
    void changed();

  private:
    QHash<QString, Offer> offers_;
    QHash<QString, QString> filled_;
    QStringList events_;
    int revision_{};
};

using namespace lapis::desktop;

QByteArray claude_thread(const char* question, const char* answer) {
    return QByteArray(R"({"type":"user","uuid":"u","message":{"content":")") + question + R"("}})" +
           "\n" +
           R"({"type":"assistant","uuid":"a","message":{"content":[{"type":"text","text":")" +
           answer + R"("}]}})" + "\n";
}

// Tab in the visual view's bar, as the owner uses it: Tab takes the guess,
// Tab again sends it unchanged, and Tab on an empty bar moves to the agent
// that most needs the person, with the bar's keyboard and that agent's guess
// ready, so the chain goes on. Edited text never goes on a Tab, a held Tab
// repeats nothing, a request is never answered, an IME composition keeps
// Tab, and with nobody else waiting a quiet note says so. Offscreen and
// software; fixture agents, a stand-in predictor and a stand-in terminal.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
int run_tab_flow(const QString& captures) {
    QTemporaryDir directory;
    require(directory.isValid(), "temporary directory");
    Workspace workspace(WorkspaceMode::fixture);
    require(workspace.addFixtureAgent("alpha", "Alpha", "general", "claude", "") &&
                workspace.addFixtureAgent("beta", "Beta", "general", "claude", "") &&
                workspace.addFixtureAgent("gamma", "Gamma", "general", "agy", "") &&
                workspace.addFixtureAgent("busy", "Busy", "general", "codex", ""),
            "sample agents");
    for (const char* id : {"alpha", "beta", "busy"}) {
        const auto path = QDir(directory.path()).filePath(QString::fromLatin1(id) + ".jsonl");
        write(path, claude_thread("Fix the parser", "Done: the parser handles tabs."));
        require(workspace.setFixtureConversation(QString::fromLatin1(id), path),
                std::string("a thread for ") + id);
    }
    QDir(directory.path()).mkpath("gamma");
    const auto gamma = QDir(directory.path()).filePath("gamma/transcript_full.jsonl");
    const auto prompt =
        step(0, "USER_INPUT", "content", "<USER_REQUEST>\nMigrate\n</USER_REQUEST>");
    write(gamma, QJsonDocument(prompt).toJson(QJsonDocument::Compact) + '\n');
    require(workspace.setFixtureConversation("gamma", gamma), "a thread for gamma");
    report(*workspace.agent("alpha"), Activity::turn_completed);
    report(*workspace.agent("beta"), Activity::turn_completed);
    report(*workspace.agent("busy"), Activity::working);
    // A fixture request stands for the agent's state only without a status
    // report (Agent::attentionCount).
    require(workspace.requestAttention({"gamma", "gate-g", "Approve: run migrate"}) &&
                workspace.agent("gamma")->statusKind() == "waiting",
            "gamma's request");
    Guesses guesses;
    // Alpha's guess says who wrote it; beta's author is unknown.
    guesses.offer("alpha", "Run the tests", "alpha:1", QStringLiteral("Fable 5.2 · high"));
    guesses.offer("beta", "Ship it", "beta:1");

    const auto config_path = QDir(directory.path()).filePath("lapis.json");
    write(config_path, R"({"version": 1})");
    Config config;
    config.setSourcePathForTesting(config_path);
    require(config.load() && config.visualAll(), "visual.all is on");
    require(workspace.selectAgent("alpha"), "start on alpha");

    AppWindow preview(workspace, {.source = QUrl::fromLocalFile(QStringLiteral(LAPIS_QML_SOURCE)),
                                  .compact = false,
                                  .screen = QString(),
                                  .config = &config,
                                  .nextPrompt = &guesses});
    require(preview.load(), "the production window loads");
    auto* window = preview.window();
    require(window != nullptr, "the window exists");
    window->resize(1200, 800);
    window->show();
    window->requestActivate();
    pump(200);
    const auto flag = [&](const char* name) { return window->property(name).toBool(); };
    const auto save = [&](const char* name) {
        if (captures.isEmpty())
            return;
        pump(250);
        const auto path = QDir(captures).filePath(QString::fromLatin1(name));
        require(window->grabWindow().save(path), "save " + path.toStdString());
        std::cout << "factory_ui_test: saved " << name << '\n';
    };
    auto* view = item(*window, "visualView");
    auto* input = qvariant_cast<QQuickItem*>(view->property("composer"));
    require(input != nullptr, "the bar's text field");
    until([&] { return flag("visualCovers") && shown(*window, "visualView"); },
          "alpha shows as its thread");
    require(QMetaObject::invokeMethod(window, "visualFocus"), "the bar takes the keyboard");
    until([&] { return input->hasActiveFocus(); }, "the bar has the keyboard");

    // The terminal underneath, standing in: what the bar sends, by agent.
    auto* engine = qmlEngine(view);
    require(engine != nullptr, "the window's engine");
    auto submit = engine->evaluate(QStringLiteral(
        "(function() { var sent = []; var f = function(text) { sent.push(text); return true };"
        " f.sent = sent; return f })()"));
    require(submit.isCallable(), "a stand-in terminal");
    view->setProperty("submit", QVariant::fromValue(submit));
    const auto sent = [&] {
        QStringList out;
        const auto list = submit.property("sent");
        for (int i = 0, n = list.property("length").toInt(); i < n; ++i)
            out.append(list.property(static_cast<quint32>(i)).toString());
        return out;
    };
    const auto focused = [&] {
        const auto* agent = workspace.focusedAgent();
        return agent != nullptr ? agent->agentId() : QString();
    };
    const auto text = [&] { return input->property("text").toString(); };
    const auto key = [&](int code, bool repeat = false, const QString& typed = {}) {
        QKeyEvent press(QEvent::KeyPress, code, Qt::NoModifier, typed, repeat);
        QKeyEvent release(QEvent::KeyRelease, code, Qt::NoModifier, typed, repeat);
        QCoreApplication::sendEvent(window, &press);
        QCoreApplication::sendEvent(window, &release);
        pump(30);
    };
    const auto tab = [&](bool repeat = false) { key(Qt::Key_Tab, repeat); };
    const auto state = [&] {
        return " (agent " + focused().toStdString() + ", text \"" + text().toStdString() +
               "\", suggestion \"" + view->property("suggestion").toString().toStdString() +
               "\", notice \"" + view->property("notice").toString().toStdString() + "\", sent [" +
               sent().join("|").toStdString() + "], events [" +
               guesses.events().join("|").toStdString() + "], bar focus " +
               std::to_string(input->hasActiveFocus()) + ")";
    };

    // Alpha offers its guess in the empty bar, and the hint says what Tab does.
    until([&] { return view->property("suggestion").toString() == "Run the tests"; },
          "alpha's guess shows" + state());
    until([&] { return guesses.events().contains("seen alpha:1"); },
          "the bar's guess counts as seen" + state());
    require(item(*window, "visualHint")->property("text").toString() ==
                "Tab to use · again to send · then the next agent",
            "the hint names the round" + state());
    const auto* author = item(*window, "visualSuggestionAuthor");
    require(author->isVisible() &&
                author->property("text").toString() == QStringLiteral("from Fable 5.2 · high"),
            "the guess says who wrote it" + state());
    save("tab_flow_offer.png");

    // Tab 1: the guess, editable, in the bar.
    tab();
    require(text() == "Run the tests" && sent().isEmpty(), "Tab 1 fills the guess" + state());
    require(guesses.events().contains("filled alpha:1"), "Tab 1 is logged as used" + state());
    // A held Tab repeats nothing: no send.
    tab(true);
    tab(true);
    require(sent().isEmpty() && text() == "Run the tests", "auto-repeat does not send" + state());

    // Tab 2: unchanged, it goes to alpha.
    tab();
    require(sent() == QStringList{"Run the tests"}, "Tab 2 sends the guess" + state());
    require(text().isEmpty() && focused() == "alpha", "the bar clears, still on alpha" + state());
    require(guesses.events().contains("sent alpha:1"), "Tab 2 is logged as sent" + state());
    tab(true);
    pump(100);
    require(focused() == "alpha", "auto-repeat does not move on" + state());

    // Tab 3: the agent that most needs the person (gamma's request), with the
    // bar's keyboard. Its CLI gets no guesses; Tab still moves.
    tab();
    until([&] { return focused() == "gamma"; }, "Tab 3 moves to the request" + state());
    until([&] { return flag("visualCovers") && input->hasActiveFocus(); },
          "gamma's bar has the keyboard" + state());
    require(view->property("waiting").toBool(), "gamma waits on its request" + state());

    // On a request Tab neither fills nor answers: an empty bar moves on.
    tab();
    until([&] { return focused() == "beta"; }, "Tab on a request moves on" + state());
    require(sent().size() == 1, "nothing was sent to the request" + state());
    until([&] { return input->hasActiveFocus(); }, "beta's bar has the keyboard" + state());
    until([&] { return view->property("suggestion").toString() == "Ship it"; },
          "beta's guess is offered at once" + state());
    require(item(*window, "visualHint")->property("text").toString() ==
                    "Tab to use · again to send · then the next agent" &&
                !item(*window, "visualSuggestionAuthor")->isVisible(),
            "a guess whose author is unknown names none" + state());
    save("tab_flow_offer_unsigned.png");

    // Edited after Tab, even back to the guess's words, the text waits for
    // Return.
    tab();
    require(text() == "Ship it" && guesses.events().contains("filled beta:1"),
            "Tab fills beta's guess" + state());
    require(item(*window, "visualHint")->property("text").toString() == "Tab or Return to send",
            "the hint says Tab sends it" + state());
    key(Qt::Key_Exclam, false, QStringLiteral("!"));
    require(text() == "Ship it!", "typed into the guess" + state());
    tab();
    require(sent().size() == 1 && text() == "Ship it!" && focused() == "beta",
            "Tab never sends edited text" + state());
    key(Qt::Key_Backspace);
    require(text() == "Ship it", "the edit undone" + state());
    tab();
    require(sent().size() == 1 && text() == "Ship it" && focused() == "beta",
            "Tab never sends text the person touched" + state());
    require(!guesses.events().contains("sent beta:1"), "nor logs it as sent" + state());
    key(Qt::Key_Exclam, false, QStringLiteral("!"));
    key(Qt::Key_Return);
    require(sent().size() == 2 && sent().at(1) == "Ship it!" && text().isEmpty(),
            "Return sends the edited text" + state());
    require(!guesses.events().contains("sent beta:1"), "a Return send is not a Tab send" + state());

    // An IME composition keeps Tab: no move.
    QInputMethodEvent compose(QStringLiteral("ni"), {});
    QCoreApplication::sendEvent(input, &compose);
    pump(30);
    require(input->property("preeditText").toString() == "ni", "a composition in the bar");
    tab();
    require(focused() == "beta", "Tab during a composition does not move" + state());
    QInputMethodEvent cancel(QString(), {});
    QCoreApplication::sendEvent(input, &cancel);
    pump(30);

    // Nobody else waiting: Tab stays and says so.
    require(workspace.resolveAttention({"gamma", "gate-g", {}}), "gamma's request answered");
    pump(50);
    tab();
    pump(100);
    require(focused() == "beta" && sent().size() == 2, "nothing waiting: Tab stays" + state());
    require(view->property("notice").toString() == "Nothing else is waiting",
            "and says nothing else waits" + state());
    save("tab_flow_nothing_waiting.png");

    require(preview.diagnostics().isEmpty(),
            "QML diagnostics: " + preview.diagnostics().toStdString());
    std::cout << "factory_ui_test: tab flow PASS (offscreen/software; stand-in predictor and "
                 "terminal; not native input, IME or GPU acceptance)\n";
    return EXIT_SUCCESS;
}
} // namespace tab_flow
#include "factory_ui_test.moc"

int main(int argc, char** argv) {
    QString captures;
    bool new_session = false;
    bool tab_chain = false;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--background")
            continue; // always: this test never shows a window
        if (argument == "--capture" && index + 1 < argc) {
            captures = QString::fromLocal8Bit(argv[++index]);
            continue;
        }
        if (argument == "--new-session") {
            new_session = true;
            continue;
        }
        if (argument == "--tab-flow") {
            tab_chain = true;
            continue;
        }
        std::cerr << "Usage: lapis_factory_ui_tests [--background] [--new-session | --tab-flow] "
                     "[--capture DIR]\n";
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
        return tab_chain     ? tab_flow::run_tab_flow(captures)
               : new_session ? run_new_session(captures)
                             : run(captures);
    } catch (const std::exception& error) {
        std::cerr << "factory_ui_test: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
