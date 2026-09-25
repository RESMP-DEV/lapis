#include "harness_models.hpp"
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

    // The new-agent forms: the CLI's list, or the config's names with the
    // CLI's default kept first.
    lapis::desktop::Workspace workspace(lapis::desktop::WorkspaceMode::preview);
    workspace.setHarnessModels(&models);
    lapis::desktop::AgentDefaults defaults;
    defaults.models.insert(QStringLiteral("claude"),
                           {QStringLiteral("claude-fable-5-1"), QStringLiteral("haiku")});
    workspace.setAgentDefaults(defaults);
    require(ids(workspace.modelChoices(QStringLiteral("codex"))) ==
                QStringList{QStringLiteral("gpt-6-astra*")},
            "the CLI's own list");
    require(ids(workspace.modelChoices(QStringLiteral("claude"))) ==
                    QStringList({QStringLiteral("claude-fable-5-1"), QStringLiteral("haiku")}) &&
                workspace.modelChoices(QStringLiteral("claude")).front().name ==
                    QStringLiteral("Fable 5.1"),
            "the config's names, named as the CLI names them");
    QStringList order;
    for (const auto& item : workspace.availableHarnesses())
        order << item.toMap().value(QStringLiteral("id")).toString();
    require(order ==
                QStringList({QStringLiteral("claude"), QStringLiteral("codex"),
                             QStringLiteral("opencode"), QStringLiteral("grok"),
                             QStringLiteral("omp"), QStringLiteral("agy"), QStringLiteral("kimi")}),
            "the pickers' order, without Gemini");
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

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        if (qEnvironmentVariableIsSet("LAPIS_MODELS_LIVE")) {
            show_live();
            return 0;
        }
        reads_each_cli_list();
        asks_each_installed_cli();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    std::cout << "harness models tests passed\n";
    return 0;
}
