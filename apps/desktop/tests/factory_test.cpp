// The factory view's model: the judge's factory_state.json, the stage rules
// that merge it with live agent state, the orchestrator choice, citation
// links, visual.factory in lapis.json and the workspace requests. No window.
#include "config.hpp"
#include "factory_state.hpp"
#include "workspace.hpp"
#include "workspace_control.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QThread>

#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
using namespace lapis::desktop;
namespace wire = lapis::session::wire;

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
bool until(const std::function<bool()>& condition, int milliseconds = 5000) {
    QElapsedTimer clock;
    clock.start();
    while (!condition() && clock.elapsed() < milliseconds)
        pump(10);
    return condition();
}
void write_file(const QString& path, const QByteArray& bytes) {
    // Written beside and renamed, as the judge does, so a reader never sees half.
    const auto partial = path + QStringLiteral(".partial");
    QFile file(partial);
    require(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "open " + partial.toStdString());
    require(file.write(bytes) == bytes.size(), "write " + partial.toStdString());
    file.close();
    QFile::remove(path);
    require(QFile::rename(partial, path), "rename into " + path.toStdString());
}
QJsonObject verdict(const char* stage, bool keep, const char* headline, const char* reason,
                    const char* receipt, qint64 judged) {
    return {{"stage", stage},   {"keepNeeded", keep}, {"headline", headline},
            {"reason", reason}, {"receipt", receipt}, {"judgedAtMs", judged}};
}
QVariantMap item_for(const Workspace& workspace, const QString& id) {
    for (const auto& value : workspace.factoryPipeline())
        if (value.toMap().value(QStringLiteral("agentId")).toString() == id)
            return value.toMap();
    return {};
}
QString stage_of(const Workspace& workspace, const QString& id) {
    return item_for(workspace, id).value(QStringLiteral("stage")).toString();
}
// A status the agent's adapter reports, applied as its service would.
void report(Agent& agent, lapis::session::attention::Activity activity) {
    wire::AttentionSnapshot snapshot;
    snapshot.available = true;
    snapshot.connected = true;
    snapshot.ready = true;
    snapshot.activity = activity;
    agent.applyAttention(snapshot);
}

void parses_the_judge_file() {
    const auto state = factory::parse(R"({
        "version": 1, "updatedAtMs": 1794466800000, "judgeModel": "claude-haiku-5-5",
        "agents": {
            "a": {"stage": "idle_cull", "keepNeeded": false, "headline": "Merged #81",
                  "reason": "Merged 2h ago, safe to close", "receipt": "225b30d · 9/9 checks",
                  "judgedAtMs": 1794466700000},
            "b": {"stage": "landed"},
            "c": {"stage": "somewhere"},
            "d": "not an object"
        }})");
    require(state.loaded && state.updated_at_ms == 1794466800000 &&
                state.judge_model == QStringLiteral("claude-haiku-5-5"),
            "the file's header is read");
    require(state.agents.size() == 2 && state.agents.contains("a") && state.agents.contains("b"),
            "unknown stages and malformed entries are skipped");
    const auto& a = state.agents["a"];
    require(a.stage == QStringLiteral("idle_cull") && !a.keep_needed &&
                a.receipt == QStringLiteral("225b30d · 9/9 checks") &&
                a.judged_at_ms == 1794466700000,
            "a verdict keeps every field");
    const auto& b = state.agents["b"];
    require(b.keep_needed && b.judged_at_ms == 1794466800000,
            "a missing keepNeeded means keep; a missing time is the file's");
    require(!factory::parse("{").loaded && !factory::parse(R"({"version": 2})").loaded &&
                !factory::parse("[]").loaded,
            "a broken or newer file is not read");
    const auto emoji = QString::fromUtf8("\xF0\x9F\x98\x80");
    const auto agent_with_key = [&emoji](int repeats) {
        return QJsonObject{
            {QStringLiteral("version"), 1},
            {QStringLiteral("agents"),
             QJsonObject{{emoji.repeated(repeats),
                          QJsonObject{{QStringLiteral("stage"), QStringLiteral("landed")}}}}}};
    };
    const auto within_bytes =
        factory::parse(QJsonDocument(agent_with_key(16)).toJson(QJsonDocument::Compact));
    const auto beyond_bytes =
        factory::parse(QJsonDocument(agent_with_key(17)).toJson(QJsonDocument::Compact));
    require(within_bytes.agents.size() == 1 && beyond_bytes.agents.isEmpty(),
            "agent keys are bounded by UTF-8 bytes, not UTF-16 units");
    const auto without_time = factory::parse(
        QJsonDocument(QJsonObject{{QStringLiteral("version"), 1},
                                  {QStringLiteral("agents"),
                                   QJsonObject{{QStringLiteral("a"),
                                                QJsonObject{{QStringLiteral("stage"),
                                                             QStringLiteral("landed")}}}}}})
            .toJson(QJsonDocument::Compact));
    require(without_time.agents.value(QStringLiteral("a")).judged_at_ms == 0 &&
                !factory::verdict_current(factory::Live{"idle", false, 0, 0, 1'000},
                                          without_time.agents.value(QStringLiteral("a"))),
            "a verdict with no timestamp is never current");
    require(!factory::read(QStringLiteral("/nonexistent/factory_state.json")).loaded,
            "a missing file is no state");
}

void merges_live_state() {
    using factory::Live;
    const qint64 now = 10'000'000;
    const auto verdict = [](const char* stage, bool keep, qint64 judged) {
        factory::Verdict result;
        result.stage = QString::fromLatin1(stage);
        result.keep_needed = keep;
        result.judged_at_ms = judged;
        return result;
    };
    require(factory::fallback_stage(Live{"waiting", true, 0, 0, now}) == "needs_you" &&
                factory::fallback_stage(Live{"working", false, 0, 0, now}) == "in_progress" &&
                factory::fallback_stage(Live{"finished", false, 0, 0, now}) == "idle_cull" &&
                factory::fallback_stage(Live{"finished", false, now - 1000, 0, now}) == "landed" &&
                factory::fallback_stage(Live{"idle", false, 0, now - 1000, now}) == "landed" &&
                factory::fallback_stage(Live{"idle", false, 0, 0, now}) == "idle_cull" &&
                factory::fallback_stage(
                    Live{"finished", false, 0, now - factory::kLandedRecentMs - 1, now}) ==
                    "idle_cull" &&
                factory::fallback_stage(Live{"ended", false, 0, 0, now}) == "idle_cull" &&
                factory::fallback_stage(Live{"unknown", false, 0, 0, now}) == "idle_cull",
            "the stage from live state alone");
    const factory::Verdict landed = verdict("landed", false, now - 500);
    require(factory::stage(Live{"idle", false, now - 900, 0, now}, &landed) == "landed",
            "a verdict newer than the agent's last change holds");
    require(factory::stage(Live{"idle", false, 0, 0, now}, &landed) == "landed",
            "a current landed verdict on an idle agent holds");
    require(factory::stage(Live{"working", false, now - 100, 0, now}, &landed) == "in_progress",
            "work started after the verdict promotes the agent at once");
    require(factory::stage(Live{"working", false, 0, 0, now}, &landed) == "in_progress",
            "a turn running at startup beats an older landed verdict");
    require(factory::stage(Live{"connecting", false, 0, 0, now}, &landed) == "in_progress",
            "a connecting agent is in progress");
    require(factory::stage(Live{"idle", true, now - 900, 0, now}, &landed) == "needs_you",
            "a pending request always needs you");
    const factory::Verdict cull = verdict("idle_cull", false, now - 500);
    require(factory::stage(Live{"waiting", false, 0, 0, now}, &cull) == "needs_you",
            "a waiting agent needs you whatever an older verdict says");
    const factory::Verdict running = verdict("in_progress", true, now - 500);
    require(factory::stage(Live{"idle", false, 0, 0, now}, &running) == "idle_cull",
            "a mid-turn verdict on an idle agent is stale");
    require(factory::stage(Live{"idle", false, 0, now - 1000, now}, &running) == "landed",
            "a mid-turn verdict on a turn that just ended falls back to landed");
    require(factory::stage(Live{"finished", false, 0, 0, now}, &running) == "idle_cull",
            "a mid-turn verdict on a finished agent with no time falls back to idle");
    require(factory::stage(Live{"ended", false, 0, 0, now}, &running) == "idle_cull",
            "a mid-turn verdict on an ended agent falls back to idle");
    require(factory::stage(Live{"working", false, 0, 0, now}, &running) == "in_progress",
            "a mid-turn verdict on a running turn agrees");
    const factory::Verdict asks = verdict("needs_you", true, now - 500);
    require(factory::stage(Live{"finished", false, now - 900, 0, now}, &asks) == "needs_you",
            "the judge can read a question in a finished reply");
    require(factory::stage(Live{"working", false, 0, 0, now}, &asks) == "in_progress",
            "a running turn beats an older question verdict");
}

void automatic_home_waits_for_activity() {
    Workspace workspace(WorkspaceMode::fixture);
    require(workspace.addFixtureAgent("first-agy", "First", "general", "agy", "first\r\n") &&
                workspace.addFixtureAgent("second-agy", "Second", "general", "agy", "second\r\n"),
            "two local Antigravity agents");
    require(workspace.effectiveHomeAgent() == workspace.agent("first-agy"),
            "without activity the registry order is only a provisional choice");
    report(*workspace.agent("second-agy"), lapis::session::attention::Activity::working);
    require(workspace.effectiveHomeAgent() == workspace.agent("second-agy"),
            "the first agent with activity becomes the automatic orchestrator");
    report(*workspace.agent("first-agy"), lapis::session::attention::Activity::working);
    require(workspace.effectiveHomeAgent() == workspace.agent("second-agy"),
            "an automatic choice with activity stays sticky");
}

void attention_inbox_keeps_a_restored_wait_older() {
    Workspace workspace(WorkspaceMode::fixture);
    const auto now = QDateTime::currentMSecsSinceEpoch();
    workspace.agent("renderer")->restoreUnseen(true, now - 5'000);
    workspace.agent("notes")->restoreUnseen(true, now - 1);
    QThread::msleep(2);
    require(workspace.requestAttention({"renderer", "restore-1", "Choose rendering"}) &&
                workspace.requestAttention({"notes", "restore-2", "Approve migration"}),
            "requests after restored marks");
    const auto inbox = workspace.attentionInbox();
    require(inbox.size() == 2 && inbox.front().toMap().value("agentId").toString() == "renderer" &&
                inbox.back().toMap().value("agentId").toString() == "notes",
            "a re-reported restored wait keeps its older place");
}

// The exact object the factory judge writes, extra fields included.
void parses_real_judge_output_schema() {
    const auto state = factory::parse(R"({
        "version": 1,
        "updatedAtMs": 1794466800000,
        "judgeModel": "claude-haiku-5-5",
        "counts": {"needs_you": 1, "in_progress": 1, "landed": 1, "idle_cull": 1,
                   "safe_to_close": 1},
        "agents": {
            "0b6f3c1e-asks": {
                "stage": "needs_you", "keepNeeded": true,
                "headline": "Asks which branch to rebase onto",
                "reason": "Last reply ends in a question", "receipt": "",
                "judgedAtMs": 1794466790000, "judgedBy": "claude-haiku-5-5",
                "statusKind": "finished", "turnFingerprint": "f00d01",
                "category": "general", "title": "Rebase helper"},
            "1c7a2d4f-runs": {
                "stage": "in_progress", "keepNeeded": true,
                "headline": "Text shaping pass", "reason": "Turn running",
                "receipt": "", "judgedAtMs": 1794466791000, "judgedBy": "claude-haiku-5-5",
                "statusKind": "working", "turnFingerprint": "f00d02",
                "category": "general", "title": "Renderer"},
            "2d8b3e5a-done": {
                "stage": "landed", "keepNeeded": true,
                "headline": "Merged #81 on main", "reason": "PR merged, checks green",
                "receipt": "225b30d · 9/9 checks", "judgedAtMs": 1794466792000,
                "judgedBy": "claude-haiku-5-5", "statusKind": "idle",
                "turnFingerprint": "f00d03", "category": "review", "title": "Checks"},
            "3e9c4f6b-cull": {
                "stage": "idle_cull", "keepNeeded": false,
                "headline": "Notes drafted", "reason": "Merged 2h ago, safe to close",
                "receipt": "", "judgedAtMs": 1794466793000, "judgedBy": "claude-haiku-5-5",
                "statusKind": "idle", "turnFingerprint": "f00d04",
                "category": "review", "title": "Notes"}
        }})");
    require(state.loaded && state.updated_at_ms == 1794466800000 &&
                state.judge_model == QStringLiteral("claude-haiku-5-5"),
            "the judge's header is read; counts are not required");
    require(state.agents.size() == 4, "every judged agent is read");
    const auto& asks = state.agents["0b6f3c1e-asks"];
    require(asks.stage == QStringLiteral("needs_you") && asks.keep_needed &&
                asks.headline == QStringLiteral("Asks which branch to rebase onto") &&
                asks.judged_at_ms == 1794466790000,
            "a needs-you verdict");
    require(state.agents["1c7a2d4f-runs"].stage == QStringLiteral("in_progress"),
            "an in-progress verdict");
    const auto& done = state.agents["2d8b3e5a-done"];
    require(done.stage == QStringLiteral("landed") &&
                done.receipt == QStringLiteral("225b30d · 9/9 checks") &&
                done.reason == QStringLiteral("PR merged, checks green"),
            "a landed verdict with its receipt");
    const auto& cull = state.agents["3e9c4f6b-cull"];
    require(cull.stage == QStringLiteral("idle_cull") && !cull.keep_needed &&
                cull.judged_at_ms == 1794466793000,
            "a safe-to-close verdict");
}

// A fixture workspace: verdicts from the file, live overrides, the orchestrator.
void pipeline_from_file_and_live_state(const QString& folder) {
    Workspace workspace(WorkspaceMode::fixture);
    require(workspace.addFixtureAgent("carbon-7a1", "Gemini 4 Carbon", "general", "agy",
                                      "orchestrator\r\n"),
            "an agy sample agent");
    require(!workspace.addFixtureAgent("carbon-7a1", "again", "general", "agy", ""),
            "ids stay unique");
    const auto path = QDir(folder).filePath(QStringLiteral("factory_state.json"));
    QFile::remove(path);
    workspace.setFactoryStatePath(path);
    require(workspace.factoryStatePath() == path, "the judge's file path");

    // Before any judge ran: deterministic stages from live state.
    require(workspace.effectiveHomeAgent() == workspace.agent("carbon-7a1"),
            "the agy agent is the orchestrator by default");
    auto items = workspace.factoryPipeline();
    require(items.size() == 6 && item_for(workspace, "carbon-7a1").isEmpty(),
            "every agent but the orchestrator is on the board");
    require(stage_of(workspace, "renderer") == "idle_cull" &&
                !item_for(workspace, "renderer").value("judged").toBool() &&
                item_for(workspace, "renderer").value("keepNeeded").toBool(),
            "an unjudged agent with no status waits in idle and is kept");
    require(workspace.factorySummary().value("idleCull").toInt() == 6 &&
                !workspace.factorySummary().value("judged").toBool(),
            "the summary counts the fallback stages");

    // The renderer's turn runs, so its in-progress verdict below agrees.
    report(*workspace.agent("renderer"), lapis::session::attention::Activity::working);
    require(stage_of(workspace, "renderer") == "in_progress",
            "a running turn is in progress before any judge ran");
    pump(5);
    const auto now = QDateTime::currentMSecsSinceEpoch();
    const auto judged = now + 1;
    const QJsonObject agents{
        {"renderer",
         verdict("in_progress", true, "Text shaping pass", "Still editing", "", judged)},
        {"checks",
         verdict("landed", true, "Merged #81 on main", "Merged", "225b30d · 9/9 checks", judged)},
        {"notes",
         verdict("idle_cull", false, "Notes drafted", "Merged 2h ago, safe to close", "", judged)},
        {"service", verdict("idle_cull", true, "Parked", "Holds the service fix", "", judged)}};
    write_file(path, QJsonDocument(QJsonObject{{"version", 1},
                                               {"updatedAtMs", now - 30'000},
                                               {"judgeModel", "claude-haiku-5-5"},
                                               {"agents", agents}})
                         .toJson());
    require(until([&] { return workspace.factorySummary().value("judged").toBool(); }),
            "the watched file loads without a reload call");
    require(stage_of(workspace, "renderer") == "in_progress" &&
                stage_of(workspace, "checks") == "landed" &&
                stage_of(workspace, "notes") == "idle_cull" &&
                stage_of(workspace, "service") == "idle_cull" &&
                stage_of(workspace, "shell") == "idle_cull",
            "the judge's stages, and the fallback for an agent it omits");
    require(item_for(workspace, "renderer").value("judged").toBool() &&
                item_for(workspace, "renderer").value("headline").toString() == "Text shaping pass",
            "a mid-turn verdict on a running turn is current");
    const auto notes = item_for(workspace, "notes");
    require(!notes.value("keepNeeded").toBool() && notes.value("judged").toBool() &&
                notes.value("reason").toString() == "Merged 2h ago, safe to close",
            "a safe-to-close verdict with its reason");
    require(item_for(workspace, "checks").value("receipt").toString() == "225b30d · 9/9 checks",
            "the receipt");
    const auto summary = workspace.factorySummary();
    require(summary.value("inProgress").toInt() == 1 && summary.value("landed").toInt() == 1 &&
                summary.value("idleCull").toInt() == 4 &&
                summary.value("safeToClose").toInt() == 1 &&
                summary.value("judgeModel").toString() == "claude-haiku-5-5",
            "the summary counts the judged stages");
    items = workspace.factoryPipeline();
    require(items.front().toMap().value("stage").toString() == "in_progress" &&
                items.back().toMap().value("stage").toString() == "idle_cull",
            "items are ordered by stage");
    for (const auto& value : items) {
        const auto entry = value.toMap();
        if (entry.value("stage").toString() != "idle_cull")
            continue;
        require(entry.value("agentId").toString() == "notes",
                "a safe-to-close agent leads its stage");
        break;
    }

    // Live state overrides an older verdict at once.
    pump(5);
    report(*workspace.agent("checks"), lapis::session::attention::Activity::working);
    require(stage_of(workspace, "checks") == "in_progress",
            "an agent that starts working after its verdict is in progress");
    const auto stale = item_for(workspace, "checks");
    require(!stale.value("judged").toBool() && stale.value("receipt").toString().isEmpty() &&
                stale.value("judgedAtMs").toLongLong() == 0,
            "a stale verdict hides its receipt and judgement time");
    require(workspace.requestAttention({"service", "approve-service", "Choose rendering"}) &&
                workspace.requestAttention({"notes", "approve-2", "Approve the migration"}),
            "fixture requests");
    pump(5);
    require(stage_of(workspace, "service") == "needs_you" &&
                stage_of(workspace, "notes") == "needs_you",
            "requests move agents to Needs you");
    const auto inbox = workspace.attentionInbox();
    require(inbox.size() == 2 && inbox.front().toMap().value("agentId").toString() == "service" &&
                inbox.back().toMap().value("agentId").toString() == "notes",
            "the gate strip keeps the first pending request first");
    require(workspace.resolveAttention({"service", "approve-service", {}}),
            "resolve the first request");
    require(workspace.resolveAttention({"notes", "approve-2", {}}), "resolve the second request");
    pump(5);
    require(workspace.attentionInbox().isEmpty(), "a resolved request leaves the strip");
    // A changed verdict reloads.
    auto next = agents;
    next.insert("service", verdict("landed", true, "Service fix merged", "", "9d1e44f",
                                   QDateTime::currentMSecsSinceEpoch() + 1));
    write_file(path, QJsonDocument(QJsonObject{{"version", 1},
                                               {"updatedAtMs", now},
                                               {"judgeModel", "claude-haiku-5-5"},
                                               {"agents", next}})
                         .toJson());
    require(until([&] { return stage_of(workspace, "service") == "landed"; }),
            "a rewritten file reloads live");

    // Citations.
    require(workspace.resolveAgentLink("lapis://agent/rend") == "renderer" &&
                workspace.resolveAgentLink("lapis://agent/CHECKS") == "checks" &&
                workspace.resolveAgentLink("lapis://agent/notes/") == "notes" &&
                workspace.resolveAgentLink("lapis://agent/s").isEmpty() &&
                workspace.resolveAgentLink("lapis://agent/missing").isEmpty() &&
                workspace.resolveAgentLink("https://example.com").isEmpty(),
            "agent links resolve by id or unique prefix");

    int home_notifications = 0;
    QObject::connect(&workspace, &Workspace::homeAgentChanged,
                     [&home_notifications] { ++home_notifications; });
    require(workspace.setHomeAgent("carbon-7a1") && workspace.homeAgentId() == "carbon-7a1" &&
                home_notifications == 1,
            "pinning the automatic orchestrator still publishes the pin");

    // Pinning another orchestrator moves it off the board.
    require(workspace.setHomeAgent("renderer") && workspace.homeAgentId() == "renderer" &&
                workspace.effectiveHomeAgent() == workspace.agent("renderer"),
            "a pinned orchestrator");
    require(item_for(workspace, "renderer").isEmpty() &&
                !item_for(workspace, "carbon-7a1").isEmpty(),
            "the pinned orchestrator leaves the board and the old one joins it");
    require(!workspace.setHomeAgent("no-such-agent") && workspace.homeAgentId() == "renderer",
            "only an open agent can be pinned");
    workspace.clearError();

    // The workspace requests answer the same.
    WorkspaceControl control(workspace, false);
    const auto ask = [&control](const QJsonObject& request) {
        return QJsonDocument::fromJson(
                   control.answerForTesting(QJsonDocument(request).toJson(QJsonDocument::Compact)))
            .object();
    };
    auto answer = ask({{"version", 1}, {"request", "factory"}});
    require(answer.value("ok").toBool() && answer.value("homeAgentId").toString() == "renderer" &&
                answer.value("items").toArray().size() == 6 &&
                answer.value("summary").toObject().value("judgeModel").toString() ==
                    "claude-haiku-5-5",
            "the factory request");
    answer = ask({{"version", 1}, {"request", "setHomeAgent"}, {"id", ""}});
    require(answer.value("ok").toBool() && answer.value("homeAgentId").toString().isEmpty() &&
                answer.value("effectiveHomeAgentId").toString() == "carbon-7a1",
            "unpinning returns to the agy orchestrator");
    answer = ask({{"version", 1}, {"request", "setHomeAgent"}, {"id", "nope"}});
    require(!answer.value("ok").toBool(), "an unknown agent is refused");
    answer = ask({{"version", 1}, {"request", "setHomeAgent"}});
    require(!answer.value("ok").toBool(), "a request without an id is refused");
}

// The pin is saved in the registry and comes back; conversation links.
void pin_persists(const QString& folder) {
    const auto canonical = QFileInfo(folder).canonicalFilePath();
    const QString first = QStringLiteral("0b6f1a4e-7d55-4c35-9a53-1f3f27b8c1a1");
    const QString second = QStringLiteral("1c7e2b5f-8e66-4d46-8b64-2a4038c9d2b2");
    const QString thread = QStringLiteral("5d0c2a9e-3b1f-4c6a-8e7d-9f0a1b2c3d4e");
    const auto record = [&](const QString& id, const char* harness) {
        return QJsonObject{{"id", id},
                           {"title", id.left(8)},
                           {"category", "general"},
                           {"endpoint", QDir(canonical).filePath(id + ".sock")},
                           {"program", "/usr/bin/true"},
                           {"harness", harness},
                           {"directory", canonical}};
    };
    auto agy = record(second, "agy");
    agy.insert("conversation", thread);
    WorkspaceOptions options;
    options.storagePath = QDir(canonical).filePath(QStringLiteral("workspace.json"));
    options.headless = true;
    {
        QFile file(options.storagePath);
        require(file.open(QIODevice::WriteOnly), "create registry");
        file.write(
            QJsonDocument(QJsonObject{{"version", 2},
                                      {"activeCategory", "general"},
                                      {"categories", QJsonArray{QJsonObject{{"id", "general"},
                                                                            {"name", "General"}}}},
                                      {"agents", QJsonArray{record(first, "codex"), agy}}})
                .toJson());
    }
    {
        Workspace workspace(WorkspaceMode::live, options);
        require(workspace.agent(first) != nullptr && workspace.agent(second) != nullptr,
                "both agents restore");
        require(workspace.factoryStatePath() ==
                    QDir(canonical).filePath(QStringLiteral("factory_state.json")),
                "the judge's file is beside the registry");
        require(workspace.effectiveHomeAgent() == workspace.agent(second),
                "the local agy agent is the orchestrator by default");
        require(workspace.resolveAgentLink("conversation://" + thread) == second &&
                    workspace.resolveAgentLink("conversation://5d0c2a9e") == second &&
                    workspace.resolveAgentLink("lapis://agent/0b6f") == first,
                "conversation and agent links resolve");
        require(workspace.setHomeAgent(first), "pin the codex agent");
    }
    {
        Workspace workspace(WorkspaceMode::live, options);
        require(workspace.homeAgentId() == first &&
                    workspace.effectiveHomeAgent() == workspace.agent(first),
                "the pin comes back after a restart");
        require(workspace.setHomeAgent({}), "unpin");
    }
    QFile saved(options.storagePath);
    require(saved.open(QIODevice::ReadOnly), "read the registry");
    require(!QJsonDocument::fromJson(saved.readAll()).object().contains("homeAgent"),
            "an unpinned registry has no homeAgent");
}

void config_keeps_factory(const QString& folder) {
    const auto path = QDir(folder).filePath(QStringLiteral("lapis.json"));
    write_file(path, R"({"version": 1})");
    {
        Config config;
        config.setSourcePathForTesting(path);
        require(config.load() && !config.visualFactory() &&
                    !config.visual().value("factory").toBool(),
                "Factory is off by default");
        require(config.setVisual("factory", true) && config.visualFactory(), "turn Factory on");
    }
    {
        QFile file(path);
        require(file.open(QIODevice::ReadOnly), "read lapis.json");
        const auto visual =
            QJsonDocument::fromJson(file.readAll()).object().value("visual").toObject();
        require(visual.value("factory").toBool(), "visual.factory is saved");
    }
    {
        Config config;
        config.setSourcePathForTesting(path);
        require(config.load() && config.visual().value("factory").toBool(),
                "visual.factory comes back after a restart");
        require(config.setVisual("factory", false), "turn Factory off");
    }
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "read lapis.json again");
    require(!QJsonDocument::fromJson(file.readAll())
                 .object()
                 .value("visual")
                 .toObject()
                 .contains("factory"),
            "off leaves no key behind");
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("lapis-factory-tests"));
    try {
        QTemporaryDir directory;
        require(directory.isValid(), "temporary directory");
        QDir(directory.path()).mkpath("fixture");
        QDir(directory.path()).mkpath("config");
        parses_the_judge_file();
        merges_live_state();
        automatic_home_waits_for_activity();
        attention_inbox_keeps_a_restored_wait_older();
        parses_real_judge_output_schema();
        pipeline_from_file_and_live_state(QDir(directory.path()).filePath("fixture"));
        // The registry's folder must be private, as QTemporaryDir makes it.
        QTemporaryDir registry;
        require(registry.isValid(), "temporary registry directory");
        pin_persists(registry.path());
        config_keeps_factory(QDir(directory.path()).filePath("config"));
    } catch (const std::exception& error) {
        std::cerr << "factory_test: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
    std::cout << "factory_test: PASS\n";
    return EXIT_SUCCESS;
}
