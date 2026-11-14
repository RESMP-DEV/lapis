#include "harness_models.hpp"
#include "transcript.hpp"
#include "workspace.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QThread>
#include <iostream>
#include <stdexcept>

namespace {
using lapis::desktop::HarnessModels;
using lapis::desktop::ModelChoice;

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

QStringList ids(const std::vector<ModelChoice>& models) {
    QStringList list;
    for (const auto& model : models)
        list << model.id + (model.isDefault ? QStringLiteral("*") : QString());
    return list;
}

QJsonObject json(const char* text) { return QJsonDocument::fromJson(text).object(); }

void write(const QString& path, const QByteArray& text) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    require(file.open(QIODevice::WriteOnly) && file.write(text) == text.size(), "write a fixture");
}

// One identity/config catalog backs the picker, launch argv, restore and
// model flags; unknown ids never become a CLI, and retired Gemini stays known.
void shares_harness_identity_and_configuration() {
    QStringList order;
    for (const auto& harness : lapis::desktop::harnesses())
        order << harness.id;
    require(order == QStringList({QStringLiteral("claude"), QStringLiteral("codex"),
                                  QStringLiteral("opencode"), QStringLiteral("grok"),
                                  QStringLiteral("omp"), QStringLiteral("agy"),
                                  QStringLiteral("kimi"), QStringLiteral("gemini")}),
            "the shared catalog keeps the CLI order, including retired Gemini");

    require(lapis::desktop::find_harness(QStringLiteral("not-a-cli")) == nullptr,
            "an unknown external harness has no descriptor");
    require(lapis::desktop::harness_program(QStringLiteral("not-a-cli")).isEmpty(),
            "an unknown external harness has no program");

    const auto* codex = lapis::desktop::find_harness(QStringLiteral("codex"));
    require(codex != nullptr && codex->offered &&
                codex->defaultArguments() ==
                    QStringList({QStringLiteral("-c"),
                                 QStringLiteral("check_for_update_on_startup=false")}) &&
                codex->resumeOption == QStringLiteral("resume") &&
                codex->modelArguments(QStringLiteral("gpt-6-astra")) ==
                    QStringList({QStringLiteral("-m"), QStringLiteral("gpt-6-astra")}),
            "Codex keeps its startup setting, native resume subcommand and short model flag");

    const auto* omp = lapis::desktop::find_harness(QStringLiteral("omp"));
    require(omp != nullptr && omp->modelArguments(QStringLiteral("provider/model")) ==
                                  QStringList{QStringLiteral("--model=provider/model")},
            "OMP keeps its assignment model flag");

    const auto* gemini = lapis::desktop::find_harness(QStringLiteral("gemini"));
    require(gemini != nullptr && !gemini->offered && !gemini->command.isEmpty() &&
                gemini->resumeOption.isEmpty() &&
                gemini->modelArguments(QStringLiteral("gemini-x")).isEmpty(),
            "retired Gemini restores by identity but offers no resume or model flag");

    require(codex->modelArguments(QString()).isEmpty(),
            "a default model starts without the model flag");
}

// Each CLI's own list, shaped as the installed CLIs gave them on September
// 24: the default first, then the CLI's order or the most recently used.
void reads_each_cli_list() {
    const auto codex = lapis::desktop::codex_models(json(R"json({"data": [
        {"model": "gpt-6-sol", "displayName": "GPT-6-Sol", "isDefault": false, "hidden": false},
        {"model": "gpt-6-astra", "displayName": "GPT-6-Astra", "isDefault": true, "hidden": false},
        {"model": "gpt-internal", "displayName": "Hidden", "isDefault": false, "hidden": true}],
        "nextCursor": null})json"));
    require(ids(codex) ==
                    QStringList({QStringLiteral("gpt-6-astra*"), QStringLiteral("gpt-6-sol")}) &&
                codex.front().name == QStringLiteral("GPT-6-Astra"),
            "Codex: its default first, hidden models left out");

    const auto claude = lapis::desktop::claude_models(json(R"json({"models": [
        {"value": "default", "displayName": "Default (recommended)", "description": "Opus 5.5 · Best for everyday, complex tasks"},
        {"value": "opus", "displayName": "Opus 5.5", "description": "Most capable"},
        {"value": "claude-fable-5-1", "displayName": "Fable 5.1", "description": "For your toughest challenges"},
        {"value": "sonnet", "displayName": "Sonnet 5"}]})json"));
    require(ids(claude) == QStringList({QStringLiteral("opus*"), QStringLiteral("claude-fable-5-1"),
                                        QStringLiteral("sonnet")}) &&
                claude[1].name == QStringLiteral("Fable 5.1"),
            "Claude: the model its default names is the default; Fable is offered");

    const auto grok = lapis::desktop::grok_models(
        QStringLiteral("You are logged in with grok.com.\n\nDefault model: "
                       "grok-4.7-build-fast\n\nAvailable models:\n"
                       "  * grok-4.7-build-fast (default)\n  - grok-4.5\n  - grok-build\n"));
    require(ids(grok) == QStringList({QStringLiteral("grok-4.7-build-fast*"),
                                      QStringLiteral("grok-4.5"), QStringLiteral("grok-build")}),
            "Grok: `grok models`, its default marked");

    const auto agy = lapis::desktop::agy_models(QStringLiteral(
        "Fetching available models...\ngemini-3.8-flash-high\tGemini 3.8 Flash (High)\n"
        "claude-sonnet-4-6\tClaude Sonnet 4.6 (Thinking)\n"));
    require(ids(agy) == QStringList({QStringLiteral("gemini-3.8-flash-high"),
                                     QStringLiteral("claude-sonnet-4-6")}) &&
                agy.front().name == QStringLiteral("Gemini 3.8 Flash (High)"),
            "Antigravity: `agy models`");

    const auto kimi = lapis::desktop::kimi_models(QStringLiteral(
        "default_model = \"kimi-code/k3\"\napi_key = "
        "\"secret\"\n\n[models.\"kimi-code/kimi-for-coding\"]\n"
        "model = \"kimi-for-coding\"\n\n[models.\"kimi-code/k3\"]\nmodel = \"k3\"\n"));
    require(ids(kimi) == QStringList({QStringLiteral("kimi-code/k3*"),
                                      QStringLiteral("kimi-code/kimi-for-coding")}) &&
                kimi.front().name == QStringLiteral("k3"),
            "Kimi: its config's model aliases, the default first");

    const auto opencode = lapis::desktop::opencode_models(json(R"json({
        "recent": [{"providerID": "zai-coding-plan", "modelID": "glm-5.2"},
                   {"providerID": "glm-local", "modelID": "glm-5.2"},
                   {"providerID": "opencode", "modelID": "big-pickle"}],
        "favorite": [{"providerID": "opencode", "modelID": "big-pickle"}]})json"));
    require(ids(opencode) == QStringList({QStringLiteral("opencode/big-pickle"),
                                          QStringLiteral("zai-coding-plan/glm-5.2"),
                                          QStringLiteral("glm-local/glm-5.2")}) &&
                opencode.front().name == QStringLiteral("big-pickle") &&
                opencode[1].name == QStringLiteral("zai-coding-plan/glm-5.2"),
            "OpenCode: favourites, then recent; a name two providers share keeps its provider");

    const auto omp = lapis::desktop::omp_models(
        QStringLiteral("theme: dark\nmodelRoles:\n  smol: openai-codex/gpt-6-astra\n  default: "
                       "inco/deepseek-v4.1-flash:fast\n"
                       "auth:\n  broker:\n    url: http://127.0.0.1:1\n"),
        {QStringLiteral("anthropic/claude-opus-5-5"),
         QStringLiteral("inco/deepseek-v4.1-flash:fast")});
    require(ids(omp) == QStringList({QStringLiteral("inco/deepseek-v4.1-flash:fast*"),
                                     QStringLiteral("openai-codex/gpt-6-astra"),
                                     QStringLiteral("anthropic/claude-opus-5-5")}) &&
                omp.front().name == QStringLiteral("deepseek-v4.1-flash:fast"),
            "OMP: its default role first, its other roles, then its recent sessions' models");
}

// `agy models` as agy 1.5.7 prints it (id, tab, name), with a family and a
// level no lapis code names: Neon, Ultra.
const char* const kAgyListing = "gemini-4-carbon-medium\tGemini 4 Carbon (Medium)\n"
                                "gemini-4-carbon-high\tGemini 4 Carbon (High)\n"
                                "gemini-4-carbon-low\tGemini 4 Carbon (Low)\n"
                                "gemini-4-hydrogen-high\tGemini 4 Hydrogen (High)\n"
                                "gemini-4-hydrogen-xhigh\tGemini 4 Hydrogen (XHigh)\n"
                                "gemini-4-hydrogen-medium\tGemini 4 Hydrogen (Medium)\n"
                                "gemini-4-hydrogen-low\tGemini 4 Hydrogen (Low)\n"
                                "gemini-pro-orchestrator\tOrchestrator\n"
                                "claude-sonnet-4-6\tClaude Sonnet 4.6 (Thinking)\n"
                                "gemini-5-neon-ultra\tGemini 5 Neon (Ultra)\n"
                                "gemini-5-neon-low\tGemini 5 Neon (Low)\n"
                                "gpt-oss-120b-medium\tGPT-OSS 120B (Medium)\n";

QString levels_of(const lapis::desktop::PickerModel& model) {
    QStringList levels;
    for (const auto& level : model.levels)
        levels << level.effort + (level.id.isEmpty() ? QString() : QLatin1Char('=') + level.id);
    return levels.join(QLatin1Char(' '));
}

// The picker: agy's ids that join a model and a level are one family with
// that family's own levels; anything else stays a model with the levels
// its CLI lists for it. Nothing here names a model or a level.
void groups_models_and_levels() {
    const auto agy =
        lapis::desktop::picker_models(lapis::desktop::agy_models(QString::fromLatin1(kAgyListing)));
    QStringList entries;
    for (const auto& model : agy)
        entries << model.id + QLatin1Char('|') + model.name;
    require(entries ==
                QStringList({QStringLiteral("gemini-4-carbon|Gemini 4 Carbon"),
                             QStringLiteral("gemini-4-hydrogen|Gemini 4 Hydrogen"),
                             QStringLiteral("gemini-pro-orchestrator|Orchestrator"),
                             QStringLiteral("claude-sonnet-4-6|Claude Sonnet 4.6 (Thinking)"),
                             QStringLiteral("gemini-5-neon|Gemini 5 Neon"),
                             QStringLiteral("gpt-oss-120b|GPT-OSS 120B")}),
            "agy: each family once, in the listing's order; a bracket that is no level stays");
    require(levels_of(agy[0]) == QStringLiteral("low=gemini-4-carbon-low "
                                                "medium=gemini-4-carbon-medium "
                                                "high=gemini-4-carbon-high"),
            "agy: Carbon's levels in order, each with its own id");
    require(levels_of(agy[1]) == QStringLiteral("low=gemini-4-hydrogen-low "
                                                "medium=gemini-4-hydrogen-medium "
                                                "high=gemini-4-hydrogen-high "
                                                "xhigh=gemini-4-hydrogen-xhigh"),
            "agy: Hydrogen alone has xhigh");
    require(agy[2].levels.empty() && agy[3].levels.empty(),
            "agy: the orchestrator and a model named with no level offer none");
    require(levels_of(agy[4]) == QStringLiteral("low=gemini-5-neon-low ultra=gemini-5-neon-ultra"),
            "agy: a new family with a new level, the unknown level after the known ones");

    // Claude Code's and Codex's listings give each model's levels.
    const auto claude =
        lapis::desktop::picker_models(lapis::desktop::claude_models(json(R"json({"models": [
        {"value": "default", "resolvedModel": "claude-opus-5-5", "displayName": "Default (recommended)",
         "description": "Use the default model (currently Opus 5.5)", "supportedEffortLevels": ["low", "high"]},
        {"value": "opus", "resolvedModel": "claude-opus-5-5", "displayName": "Opus",
         "supportedEffortLevels": ["low", "medium", "high", "xhigh", "max"]},
        {"value": "claude-fable-6-0", "resolvedModel": "claude-fable-6-0", "displayName": "Fable 6",
         "supportedEffortLevels": ["low", "turbo"]}]})json")));
    require(claude.size() == 3 && claude[1].id == QStringLiteral("opus") &&
                claude[1].resolved == QStringLiteral("claude-opus-5-5") &&
                levels_of(claude[1]) == QStringLiteral("low medium high xhigh max") &&
                levels_of(claude[2]) == QStringLiteral("low turbo") &&
                levels_of(claude[0]) == QStringLiteral("low high"),
            "Claude: supportedEffortLevels and resolvedModel per model, new ones as listed");
    const auto codex =
        lapis::desktop::picker_models(lapis::desktop::codex_models(json(R"json({"data": [
        {"model": "gpt-7", "displayName": "GPT-7", "isDefault": true, "supportedReasoningEfforts": [
            {"reasoningEffort": "low"}, {"reasoningEffort": "ultra"}, {"reasoningEffort": "high"}]}]})json")));
    require(codex.size() == 1 && levels_of(codex[0]) == QStringLiteral("low high ultra"),
            "Codex: supportedReasoningEfforts per model");
}

template <typename Done> void wait_for(Done done, const char* what) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (!done() && elapsed.elapsed() < 15000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
    require(done(), what);
}

// Each installed CLI is asked, files are read for the rest, and a CLI not
// installed offers nothing.
void asks_each_installed_cli() {
    QTemporaryDir dir;
    require(dir.isValid(), "temporary directory");
    QHash<QString, QString> programs;
    const auto fake = [&](const QString& name, const QByteArray& body) {
        const auto path = dir.filePath(QStringLiteral("bin/") + name);
        write(path, "#!/bin/sh\n" + body);
        QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                        QFileDevice::ExeOwner);
        programs.insert(name, path);
    };
    fake(QStringLiteral("codex"), R"(read -r line
echo '{"id":1,"result":{}}'
read -r line
read -r line
case "$line" in *model/list*) ;; *) exit 3 ;; esac
echo '{"id":2,"result":{"data":[{"model":"gpt-6-astra","displayName":"GPT-6-Astra","isDefault":true}]}}'
cat >/dev/null
)");
    fake(QStringLiteral("claude"), R"(read -r line
case "$line" in *initialize*) ;; *) exit 3 ;; esac
echo '{"type":"control_response","response":{"subtype":"success","request_id":"models","response":{"models":[{"value":"opus","displayName":"Opus 5.5"},{"value":"claude-fable-5-1","displayName":"Fable 5.1"}]}}}'
cat >/dev/null
)");
    fake(QStringLiteral("grok"),
         "printf 'Available models:\\n  * grok-4.7 (default)\\n  - grok-4.5\\n'\n");
    fake(QStringLiteral("kimi"), "exit 0\n");
    fake(QStringLiteral("omp"), "exit 0\n");
    const auto home = dir.filePath(QStringLiteral("home"));
    write(home + QStringLiteral("/.kimi-code/config.toml"),
          "default_model = \"kimi-code/k3\"\n[models.\"kimi-code/k3\"]\n");
    write(home + QStringLiteral("/.omp/agent/config.yml"),
          "modelRoles:\n  default: inco/deepseek:fast\n");
    write(home + QStringLiteral("/.omp/agent/sessions/-w-/2026-09-24.jsonl"),
          R"({"type":"session"})"
          "\n"
          R"({"type":"model_change","model":"anthropic/claude-opus-5-5"})"
          "\n");
    qputenv("KIMI_CODE_HOME", QByteArray());
    qputenv("XDG_STATE_HOME", QByteArray());
    HarnessModels models([&](const QString& id) { return programs.value(id); }, home);
    models.refresh();
    wait_for(
        [&] {
            return !models.models(QStringLiteral("codex")).empty() &&
                   !models.models(QStringLiteral("claude")).empty() &&
                   !models.models(QStringLiteral("grok")).empty() &&
                   !models.models(QStringLiteral("kimi")).empty() &&
                   !models.models(QStringLiteral("omp")).empty();
        },
        "every installed CLI's models arrive");
    require(ids(models.models(QStringLiteral("claude"))) ==
                QStringList({QStringLiteral("opus"), QStringLiteral("claude-fable-5-1")}),
            "Claude's list from initialize");
    require(ids(models.models(QStringLiteral("omp"))) ==
                QStringList({QStringLiteral("inco/deepseek:fast*"),
                             QStringLiteral("anthropic/claude-opus-5-5")}),
            "OMP: its role and its newest session's model");
    require(models.models(QStringLiteral("opencode")).empty() &&
                models.models(QStringLiteral("agy")).empty(),
            "a CLI not installed offers nothing");

    // The new-agent forms: the CLI's list, or the config's names as written.
    lapis::desktop::Workspace workspace(lapis::desktop::WorkspaceMode::fixture);
    workspace.setHarnessModels(&models);
    lapis::desktop::AgentDefaults defaults;
    defaults.models.insert(QStringLiteral("claude"),
                           {QStringLiteral("claude-fable-5-1"), QStringLiteral("haiku"),
                            QStringLiteral("custom model [v2]"), QStringLiteral("haiku")});
    workspace.setAgentDefaults(defaults);
    require(ids(workspace.modelChoices(QStringLiteral("codex"))) ==
                QStringList{QStringLiteral("gpt-6-astra*")},
            "the CLI's own list");
    require(ids(workspace.modelChoices(QStringLiteral("claude"))) ==
                    QStringList({QStringLiteral("claude-fable-5-1"), QStringLiteral("haiku"),
                                 QStringLiteral("custom model [v2]")}) &&
                workspace.modelChoices(QStringLiteral("claude")).front().name ==
                    QStringLiteral("Fable 5.1"),
            "the config's names, named as the CLI names them");
    // A configured list keeps its order even when it names the CLI's default
    // second, and none is marked default, so the form picks the first.
    defaults.models.insert(QStringLiteral("codex"),
                           {QStringLiteral("gpt-6-sol"), QStringLiteral("gpt-6-astra")});
    workspace.setAgentDefaults(defaults);
    const auto codex = workspace.modelChoices(QStringLiteral("codex"));
    require(ids(codex) ==
                    QStringList({QStringLiteral("gpt-6-sol"), QStringLiteral("gpt-6-astra")}) &&
                codex.back().name == QStringLiteral("GPT-6-Astra"),
            "the config's order, without the CLI's default first or flagged");
    QStringList order;
    for (const auto& item : workspace.availableHarnesses())
        order << item.toMap().value(QStringLiteral("id")).toString();
    require(order ==
                QStringList({QStringLiteral("claude"), QStringLiteral("codex"),
                             QStringLiteral("opencode"), QStringLiteral("grok"),
                             QStringLiteral("omp"), QStringLiteral("agy"), QStringLiteral("kimi")}),
            "the pickers' order, without Gemini");
}

// A stand-in agy whose listing changes while lapis runs: a refresh shows
// the new list, every entry of it (no cap), the workspace's revision moves
// for the bindings, and a failed ask keeps the list it had.
void refreshes_without_restart() {
    QTemporaryDir dir;
    require(dir.isValid(), "temporary directory");
    const auto listing = dir.filePath(QStringLiteral("listing"));
    const auto agy = dir.filePath(QStringLiteral("bin/agy"));
    // Prints the listing file; "fail" in it is an error and no list.
    write(agy, "#!/bin/sh\ncase \"$1\" in models) ;; *) exit 3 ;; esac\n"
               "if grep -q fail \"" +
                   listing.toUtf8() +
                   "\"; then echo 'error: signed out' >&2; exit 1; fi\n"
                   "cat \"" +
                   listing.toUtf8() + "\"\n");
    QFile::setPermissions(agy,
                          QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    write(listing, "gemini-4-carbon-high\tGemini 4 Carbon (High)\n");
    HarnessModels models(
        [&](const QString& id) { return id == QLatin1String("agy") ? agy : QString(); },
        dir.filePath(QStringLiteral("home")));
    lapis::desktop::Workspace workspace(lapis::desktop::WorkspaceMode::fixture);
    workspace.setHarnessModels(&models);
    int changes = 0;
    QObject::connect(&models, &HarnessModels::changed, [&] { ++changes; });
    const auto agy_ids = [&] { return ids(models.models(QStringLiteral("agy"))); };

    require(models.refreshIfOlder(), "the first ask goes out");
    wait_for([&] { return !agy_ids().isEmpty(); }, "the first listing arrives");
    require(agy_ids() == QStringList{QStringLiteral("gemini-4-carbon-high")}, "the first listing");
    require(!models.refreshIfOlder(), "a list under a minute old is not asked for again");
    const auto revision = workspace.modelsRevision();

    write(listing, kAgyListing);
    require(models.refreshIfOlder(0), "an older list is asked for again");
    wait_for([&] { return agy_ids().size() == 12; }, "the changed listing arrives");
    require(workspace.modelsRevision() > revision, "the workspace's bindings hear of it");
    const auto picker = workspace.pickerModels(QStringLiteral("agy"));
    QStringList names;
    for (const auto& entry : picker)
        names << entry.toMap().value(QStringLiteral("name")).toString();
    require(names.contains(QStringLiteral("Gemini 4 Hydrogen")) &&
                names.contains(QStringLiteral("Gemini 5 Neon")) && names.size() == 6,
            "the picker has every family of the new listing");
    QVariantList form;
    for (const auto& item : workspace.availableHarnesses())
        if (item.toMap().value(QStringLiteral("id")).toString() == QLatin1String("agy"))
            form = item.toMap().value(QStringLiteral("models")).toList();
    require(form.size() == HarnessModels::kMostShown,
            "the new-agent form still shows the first few");

    // A failed ask (an error, no list) keeps what was shown.
    write(listing, "fail\n");
    const auto before = changes;
    require(models.refreshIfOlder(0), "asked again");
    QElapsedTimer waited;
    waited.start();
    while (waited.elapsed() < 1500) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
    require(changes == before && agy_ids().size() == 12, "a failed ask keeps the previous list");
}

// Opt-in: what the installed CLIs list for this person.
void show_live() {
    HarnessModels models(&lapis::desktop::harness_program, QDir::homePath());
    QElapsedTimer timer;
    timer.start();
    models.refresh();
    qint64 quiet = 0;
    QObject::connect(&models, &HarnessModels::changed, [&] { quiet = timer.elapsed(); });
    while (timer.elapsed() < 60000 && timer.elapsed() - quiet < 5000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(10);
    }
    std::cout << "live: settled after " << quiet << " ms\n";
    for (const auto* id : {"claude", "codex", "opencode", "grok", "omp", "agy", "kimi"}) {
        std::cout << id << ':';
        for (const auto& model : models.models(QString::fromLatin1(id)))
            std::cout << ' ' << model.name.toStdString() << (model.isDefault ? " (default)" : "")
                      << ';';
        std::cout << '\n';
    }
}
} // namespace

namespace {
// What an agent runs before its file says: its launch words (local, and the
// words of a remote command there), else its CLI's default here; never a
// remote machine's default. Stand-in CLIs, never a real one.
void knows_what_an_agent_runs() {
    using lapis::desktop::launched_model;
    const auto words = [](const char* text) {
        return QString::fromUtf8(text).split(QLatin1Char(' '), Qt::SkipEmptyParts);
    };
    const auto launched = [](const char* model, const char* effort) {
        return lapis::desktop::LaunchedModel{QString::fromUtf8(model), QString::fromUtf8(effort)};
    };
    require(launched_model(QStringLiteral("claude"),
                           words("--dangerously-skip-permissions --model opus --effort xhigh")) ==
                launched("opus", "xhigh"),
            "Claude Code: --model and --effort");
    require(launched_model(QStringLiteral("claude"),
                           words("--model=sonnet --model claude-fable-5-2[1m] --effort=low")) ==
                launched("claude-fable-5-2[1m]", "low"),
            "the last one wins, = forms too");
    require(launched_model(QStringLiteral("claude"), words("--resume abc -- --model opus")) ==
                launched("", ""),
            "nothing after --");
    require(launched_model(QStringLiteral("codex"),
                           words("-c notify=x -m gpt-6-sol -c model_reasoning_effort=high")) ==
                launched("gpt-6-sol", "high"),
            "Codex: -m and its effort setting");
    require(launched_model(QStringLiteral("codex"),
                           {QStringLiteral("--config"), QStringLiteral("model=\"gpt-6-astra\""),
                            QStringLiteral("--config=model_reasoning_effort='low'")}) ==
                launched("gpt-6-astra", "low"),
            "Codex: a model and level set as TOML");
    require(launched_model(QStringLiteral("agy"), words("--model gemini-4-hydrogen-xhigh")) ==
                launched("gemini-4-hydrogen-xhigh", ""),
            "agy: its model id");
    require(launched_model(QStringLiteral("codex"), words("resume 0199a000")) == launched("", ""),
            "none given");

    QTemporaryDir dir;
    require(dir.isValid(), "temporary directory");
    QHash<QString, QString> programs;
    const auto fake = [&](const QString& name, const QByteArray& body) {
        const auto path = dir.filePath(QStringLiteral("bin/") + name);
        write(path, "#!/bin/sh\n" + body);
        QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                        QFileDevice::ExeOwner);
        programs.insert(name, path);
    };
    fake(QStringLiteral("claude"), R"sh(read -r line
case "$line" in *initialize*) ;; *) exit 3 ;; esac
echo '{"type":"control_response","response":{"subtype":"success","request_id":"models","response":{"models":[{"value":"default","displayName":"Default (recommended)","description":"Fable 5.2 · Best for everyday tasks","resolvedModel":"claude-fable-5-2"},{"value":"opus","displayName":"Opus 5.5","resolvedModel":"claude-opus-5-5","supportedEffortLevels":["low","high","xhigh"]}]}}}'
cat >/dev/null
)sh");
    fake(QStringLiteral("codex"), R"(read -r line
echo '{"id":1,"result":{}}'
read -r line
read -r line
case "$line" in *model/list*) ;; *) exit 3 ;; esac
echo '{"id":2,"result":{"data":[{"model":"gpt-6-sol","displayName":"GPT-6 Sol"},{"model":"gpt-6-astra","displayName":"GPT-6 Astra","isDefault":true}]}}'
cat >/dev/null
)");
    fake(QStringLiteral("agy"), "case \"$1\" in models) ;; *) exit 3 ;; esac\n"
                                "printf 'gemini-4-hydrogen-high\\tGemini 4 Hydrogen (High)\\n"
                                "gemini-4-hydrogen-xhigh\\tGemini 4 Hydrogen (XHigh)\\n'\n");
    const auto home = dir.filePath(QStringLiteral("home"));
    QDir().mkpath(home);
    HarnessModels models([&](const QString& id) { return programs.value(id); }, home);
    models.refresh();
    wait_for(
        [&] {
            return !models.models(QStringLiteral("claude")).empty() &&
                   !models.models(QStringLiteral("codex")).empty() &&
                   !models.models(QStringLiteral("agy")).empty();
        },
        "the stand-in lists arrive");

    lapis::desktop::Workspace workspace(lapis::desktop::WorkspaceMode::fixture);
    workspace.setHarnessModels(&models);
    auto* transcript = qobject_cast<lapis::desktop::TranscriptModel*>(workspace.transcript());
    require(transcript != nullptr, "the workspace's thread");
    const auto settings = dir.filePath(QStringLiteral("agy-settings.json"));
    transcript->setAntigravitySettings(settings);
    const auto add = [&](const char* id, const char* harness, const QString& program,
                         const QStringList& arguments) {
        require(workspace.addFixtureAgent(QString::fromUtf8(id), QString::fromUtf8(id),
                                          QStringLiteral("general"), QString::fromUtf8(harness),
                                          {}) &&
                    workspace.setFixtureLaunch(QString::fromUtf8(id), program, arguments),
                "a fixture agent");
    };
    const auto known = [&](const char* id) {
        const auto map = workspace.agentModel(QString::fromUtf8(id));
        return QStringList{map.value(QStringLiteral("model")).toString(),
                           map.value(QStringLiteral("effort")).toString(),
                           map.value(QStringLiteral("resolved")).toString(),
                           map.value(QStringLiteral("from")).toString()};
    };
    const auto expect = [](const char* model, const char* effort, const char* resolved,
                           const char* from) {
        return QStringList{QString::fromUtf8(model), QString::fromUtf8(effort),
                           QString::fromUtf8(resolved), QString::fromUtf8(from)};
    };
    add("alias", "claude", programs.value(QStringLiteral("claude")),
        words("--model opus --effort xhigh"));
    require(known("alias") == expect("opus", "xhigh", "claude-opus-5-5", "launch"),
            "a local Claude Code started with an alias: its API id from the list");
    add("plain", "claude", programs.value(QStringLiteral("claude")),
        words("--dangerously-skip-permissions"));
    require(known("plain") == expect("claude-fable-5-2", "", "", "list"),
            "none given: the model Claude Code's default entry names");
    add("codex", "codex", programs.value(QStringLiteral("codex")),
        words("-c model_reasoning_effort=medium"));
    require(known("codex") == expect("gpt-6-astra", "medium", "", "list"),
            "Codex with a level only: its listed default and that level");
    add("codex-sol", "codex", programs.value(QStringLiteral("codex")), words("-m gpt-6-sol"));
    require(known("codex-sol") == expect("gpt-6-sol", "", "", "launch"), "Codex's -m");
    add("agy-id", "agy", programs.value(QStringLiteral("agy")),
        words("--model gemini-4-hydrogen-xhigh"));
    require(known("agy-id") ==
                expect("gemini-4-hydrogen-xhigh", "xhigh", "Gemini 4 Hydrogen", "launch"),
            "agy: its level from the id, its family's name as its file reports it");
    add("agy-saved", "agy", programs.value(QStringLiteral("agy")), {});
    require(known("agy-saved") ==
                expect("gemini-4-hydrogen-high", "high", "Gemini 4 Hydrogen", "list"),
            "agy with no saved default: the first entry");
    write(settings, R"json({"model":"Gemini 4 Hydrogen (XHigh)"})json");
    require(known("agy-saved") ==
                expect("gemini-4-hydrogen-xhigh", "xhigh", "Gemini 4 Hydrogen", "settings"),
            "agy's saved default, as its level's id");

    // Remote: the words inside the ssh command, quoted as lapis writes them.
    const QStringList ssh_options{QStringLiteral("-o"), QStringLiteral("ControlPath=none")};
    const auto remote_claude =
        QStringLiteral(R"(cd '/work/x' && s=0199a000-0000-7000-8000-00000000c0de && )"
                       R"(o=--session-id && { exec "${SHELL:-/bin/sh}" -lic )"
                       R"('export CLAUDE_CODE_NO_FLICKER="${CLAUDE_CODE_NO_FLICKER:-0}"; )"
                       R"(claude --model '\''opus[1m]'\'' --effort high '"$o $s"; })");
    add("remote", "claude", QStringLiteral("/usr/bin/ssh"),
        ssh_options + QStringList{QStringLiteral("-t"), QStringLiteral("box"), remote_claude});
    require(known("remote") == expect("opus[1m]", "high", "", "launch"),
            "a remote Claude Code's launch words inside its ssh command");
    const auto remote_codex = QStringLiteral(
        R"(cd '/work/x' && exec "${SHELL:-/bin/sh}" -lic 'codex -m gpt-6-sol -c model_reasoning_effort=low')");
    add("remote-codex", "codex", QStringLiteral("/usr/bin/ssh"),
        ssh_options + QStringList{QStringLiteral("-t"), QStringLiteral("box"), remote_codex});
    require(known("remote-codex") == expect("gpt-6-sol", "low", "", "launch"),
            "a remote Codex's launch words");
    const auto remote_plain =
        QStringLiteral(R"(cd '/work/x' && exec "${SHELL:-/bin/sh}" -lic 'codex')");
    add("remote-plain", "codex", QStringLiteral("/usr/bin/ssh"),
        ssh_options + QStringList{QStringLiteral("-t"), QStringLiteral("box"), remote_plain});
    require(workspace.agentModel(QStringLiteral("remote-plain")).isEmpty(),
            "a remote agent with no model: this Mac's default is not that machine's");
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        if (qEnvironmentVariableIsSet("LAPIS_MODELS_LIVE")) {
            show_live();
            return 0;
        }
        reads_each_cli_list();
        groups_models_and_levels();
        shares_harness_identity_and_configuration();
        asks_each_installed_cli();
        refreshes_without_restart();
        knows_what_an_agent_runs();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    std::cout << "harness models tests passed\n";
    return 0;
}
