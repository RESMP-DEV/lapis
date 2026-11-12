// NextPrompt predicts what the person types next to an agent that finished a
// turn: the helper reads the conversation where the agent runs ("devbox"
// through a stand-in ssh), then predicts on this Mac. Stand-ins for python3
// and ssh record what they were given and print canned answers.
#include "next_prompt.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QTemporaryDir>
#include <QVariantMap>
#include <lapis/session/terminal.hpp>

#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>

using lapis::desktop::NextPrompt;
using lapis::desktop::NextPromptSettings;

namespace {
constexpr qsizetype kComparedForTest = 2000;
NextPromptSettings on(int max_per_hour = 60, const QString& effort = {}) {
    NextPromptSettings settings;
    settings.automatic = true;
    settings.maxPerHour = max_per_hour;
    settings.effort = effort;
    settings.contextArms = {QStringLiteral("base")};
    return settings;
}
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
bool waitFor(const std::function<bool()>& done, int ms = 10000) {
    QElapsedTimer clock;
    clock.start();
    while (!done() && clock.elapsed() < ms)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return done();
}
QByteArray read(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
void write(const QString& path, const QByteArray& text) {
    QFile file(path);
    require(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "write a fixture file");
    file.write(text);
}
QList<QJsonObject> events(const QString& log) {
    QList<QJsonObject> found;
    for (const auto& line : read(log).split('\n'))
        if (!line.isEmpty())
            found << QJsonDocument::fromJson(line).object();
    return found;
}

// python3 answers `context` and `predict` from files; ssh answers `context`.
// Each records its arguments and stdin by mode.
void standIns(const QDir& root) {
    const auto python = root.filePath(QStringLiteral("bin/python3"));
    write(python, QStringLiteral("#!/bin/sh\nmode=$2\nprintf '%s\\n' \"$@\" > '%1/'$mode.args\n"
                                 "cat > '%1/'$mode.stdin\ncat '%1/'$mode.reply\n")
                      .arg(root.path())
                      .toUtf8());
    const auto ssh = root.filePath(QStringLiteral("bin/ssh"));
    write(ssh, QStringLiteral("#!/bin/sh\nprintf '%s\\n' \"$@\" > '%1/ssh.args'\n"
                              "cat > '%1/ssh.stdin'\ncat '%1/context.reply'\n")
                   .arg(root.path())
                   .toUtf8());
    for (const auto& path : {python, ssh})
        require(QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner),
                "make a stand-in executable");
}

QString fixtureProgram(const QDir& root, const QString& name) {
    if (name == QLatin1String("claude") &&
        QFileInfo::exists(root.filePath(QStringLiteral("no-claude"))))
        return {};
    return root.filePath(QStringLiteral("bin/") + name);
}

void settingsReadFromTheConfig() {
    const auto defaults = lapis::desktop::parse_next_prompt(QJsonValue());
    require(defaults == NextPromptSettings{} && !defaults.automatic &&
                defaults.model == QLatin1String("claude-opus-5-5") && defaults.maxPerHour == 60,
            "off by default, Opus, sixty an hour");
    const auto set = lapis::desktop::parse_next_prompt(
        QJsonDocument::fromJson(R"({"auto": true, "model": "claude-sonnet-5", "effort": "low",
                                    "maxPerHour": -3})")
            .object());
    require(set.automatic && set.model == QLatin1String("claude-sonnet-5") &&
                set.effort == QLatin1String("low") && set.maxPerHour == 0,
            "each field, clamped");
    require(defaults.contextArms == QStringList{QStringLiteral("base")} &&
                defaults.returnMinutes == 30,
            "the base context by default: no wide-candidate cost");
    const auto arms = lapis::desktop::parse_next_prompt(
        QJsonDocument::fromJson(R"({"contextArms": ["wide", "bogus", "wide"],
                                    "returnMinutes": 0})")
            .object());
    require(arms.contextArms == QStringList{QStringLiteral("wide")} && arms.returnMinutes == 1,
            "known arms once each, a clamped return setting");
    const auto none = lapis::desktop::parse_next_prompt(
        QJsonDocument::fromJson(R"({"contextArms": []})").object());
    require(none.contextArms == QStringList{QStringLiteral("base")}, "no arms: the base one");
    // The finished experiments' settings and arms load as if absent.
    const auto retired = lapis::desktop::parse_next_prompt(
        QJsonDocument::fromJson(
            R"({"contextArms": ["legacy", "thread", "retrieval", "evidence", "moves", "pack"],
                "experiment": 0.5, "experimentModels": ["claude-sonnet-5-5"],
                "localEndpoint": "http://127.0.0.1:8000/v1", "localModel": "local",
                "holdout": 0.2, "minConfidence": 0.4, "experimentMinConfidence": 0.4,
                "returnShare": 0.5})")
            .object());
    require(retired == NextPromptSettings{}, "the retired experiments' settings are ignored");
    require(defaults.promptArms == QStringList({QStringLiteral("base"), QStringLiteral("checks")}),
            "the prompt checks are compared half and half by default");
    const auto prompts = lapis::desktop::parse_next_prompt(
        QJsonDocument::fromJson(R"({"promptArms": ["checks", "bogus", "checks"]})").object());
    require(prompts.promptArms == QStringList{QStringLiteral("checks")},
            "known prompt arms once each");
    const auto no_prompts = lapis::desktop::parse_next_prompt(
        QJsonDocument::fromJson(R"({"promptArms": []})").object());
    require(no_prompts.promptArms ==
                QStringList({QStringLiteral("base"), QStringLiteral("checks")}),
            "no prompt arms: the documented default comparison");
    const auto chain = lapis::desktop::parse_next_prompt(QJsonDocument::fromJson(R"({"providers": [
            {"url": "https://one.test/v1", "model": "deepseek", "name": "one",
             "apiKeyEnv": "KEY", "timeoutSeconds": 0, "maxTokens": 1,
             "temperature": 9},
            {"url": "file:///etc/passwd", "model": "bad"},
            {"url": "https://two.test/v1", "model": "two"},
            {"url": "https://three.test/v1", "model": "three",
             "timeoutSeconds": 30.5, "maxTokens": 4096.5},
            {"url": "https://four.test/v1", "model": "four", "timeoutSeconds": 999},
            {"url": "http://lan.test/v1", "model": "lan", "apiKeyEnv": "K",
             "allowInsecureHttp": true}
        ]})")
                                                             .object());
    require(
        chain.providers.size() == 5 &&
            chain.providers.at(0).toObject().value(QStringLiteral("name")).toString() ==
                QLatin1String("one") &&
            chain.providers.at(0).toObject().value(QStringLiteral("timeoutSeconds")).toInt() == 1 &&
            chain.providers.at(0).toObject().value(QStringLiteral("maxTokens")).toInt() == 128 &&
            chain.providers.at(0).toObject().value(QStringLiteral("temperature")).toDouble() ==
                2.0 &&
            chain.providers.at(1).toObject().value(QStringLiteral("name")).toString() ==
                QLatin1String("two") &&
            chain.providers.at(2).toObject().value(QStringLiteral("timeoutSeconds")).toInt() ==
                31 &&
            chain.providers.at(2).toObject().value(QStringLiteral("maxTokens")).toInt() == 4097 &&
            chain.providers.at(3).toObject().value(QStringLiteral("timeoutSeconds")).toInt() ==
                60 &&
            chain.providers.at(4).toObject().value(QStringLiteral("allowInsecureHttp")).toBool() ==
                true,
        "providers are validated, clamped and named after their model");
}

void screensReadAsText() {
    lapis::session::Terminal terminal({12, 4});
    terminal.feed("hi\r\n> go   ");
    require(lapis::desktop::terminal_screen_text(terminal.snapshot()) == QLatin1String("hi\n> go"),
            "rows without trailing blanks or blank last rows");
    lapis::session::Terminal large({1000, 7});
    std::string emoji;
    for (int i = 0; i < 3500; ++i)
        emoji += "\xf0\x9f\x98\x80";
    large.feed(emoji);
    const auto clipped = lapis::desktop::terminal_screen_text(large.snapshot());
    require(!clipped.isEmpty() && !clipped.front().isLowSurrogate(),
            "the screen text budget must retain complete Unicode scalars");
}

void concurrentContextsRespectCap() {
    QTemporaryDir directory;
    const QDir root(directory.path());
    require(root.mkpath(QStringLiteral("bin")), "fixture bin");
    standIns(root);
    write(root.filePath(QStringLiteral("context.reply")), R"({"conversation":"c"})");
    write(root.filePath(QStringLiteral("predict.reply")),
          R"({"candidates":[{"text":"go","p":0.8}]})");
    const auto log = root.filePath(QStringLiteral("next.jsonl"));
    NextPrompt next(
        [](const QString&) -> std::optional<NextPrompt::Agent> {
            NextPrompt::Agent agent;
            agent.cli = QStringLiteral("claude");
            return agent;
        },
        [] { return QJsonArray{}; },
        [&root](const QString& name) { return root.filePath(QStringLiteral("bin/") + name); },
        {root.path(), log});
    next.setSettings(on(1));
    next.turnFinished(QStringLiteral("a"));
    next.turnFinished(QStringLiteral("b"));
    require(waitFor([&] { return events(log).size() == 2; }), "both contexts finish");
    int predicted = 0;
    int skipped = 0;
    for (const auto& event : events(log)) {
        predicted += event.value(QStringLiteral("event")) == QLatin1String("predicted");
        skipped += event.value(QStringLiteral("event")) == QLatin1String("skipped");
    }
    require(predicted == 1 && skipped == 1, "concurrent contexts must share the prediction cap");
}

void missingProbabilityIsNotOffered() {
    QTemporaryDir directory;
    const QDir root(directory.path());
    require(root.mkpath(QStringLiteral("bin")), "fixture bin");
    standIns(root);
    const auto log = root.filePath(QStringLiteral("next.jsonl"));
    NextPrompt next(
        [](const QString&) -> std::optional<NextPrompt::Agent> {
            NextPrompt::Agent agent;
            agent.cli = QStringLiteral("claude");
            return agent;
        },
        [] { return QJsonArray{}; },
        [&root](const QString& name) { return root.filePath(QStringLiteral("bin/") + name); },
        {root.path(), log});
    next.setSettings(on(60));
    // Absent and non-numeric probabilities are unknown; a reported zero is a
    // real probability the model gave and is offered at the default.
    for (const auto* reply :
         {R"({"candidates":[{"text":"go"}]})", R"({"candidates":[{"text":"go","p":"0.9"}]})"}) {
        write(root.filePath(QStringLiteral("context.reply")), R"({"conversation":"c"})");
        write(root.filePath(QStringLiteral("predict.reply")), reply);
        const auto before = events(log).size();
        next.turnFinished(QStringLiteral("a"));
        require(waitFor([&] { return events(log).size() > before; }), "the prediction is logged");
        require(next.suggestion(QStringLiteral("a")).isEmpty() &&
                    !events(log).last().value(QStringLiteral("shown")).toBool(),
                "a missing or non-numeric probability is unknown, not an offer");
    }
    write(root.filePath(QStringLiteral("context.reply")), R"({"conversation":"c"})");
    write(root.filePath(QStringLiteral("predict.reply")),
          R"({"candidates":[{"text":"go","p":0.0}]})");
    const auto before_zero = events(log).size();
    next.turnFinished(QStringLiteral("a"));
    require(waitFor([&] { return events(log).size() > before_zero; }),
            "the zero-probability prediction is logged");
    require(next.suggestion(QStringLiteral("a")) == QLatin1String("go"),
            "a model-reported zero is offered at the default threshold");
}

// The shown offer, for other apps (AgentStatePublisher): its key, text, the
// reply it answers, and whether it was seen, announced once. Leaves it seen.
void offerIsPublished(NextPrompt& next, const QString& key) {
    const auto published = next.offerState(QStringLiteral("a"));
    require(published.value(QStringLiteral("key")) == key &&
                published.value(QStringLiteral("text")) == QLatin1String("go") &&
                !published.value(QStringLiteral("seen")).toBool() &&
                published.value(QStringLiteral("said")) == QLatin1String("Done.") &&
                next.offerState(QStringLiteral("b")).isEmpty(),
            "the offer is published with the reply it answers");
    int seen_changes = 0;
    const auto counted =
        QObject::connect(&next, &NextPrompt::seenChanged, [&seen_changes] { ++seen_changes; });
    next.seen(QStringLiteral("a"));
    next.seen(QStringLiteral("a"));
    QObject::disconnect(counted);
    require(seen_changes == 1 &&
                next.offerState(QStringLiteral("a")).value(QStringLiteral("seen")).toBool(),
            "being seen is announced once");
}

void predictsAndOffers() {
    QTemporaryDir directory(QStringLiteral("/tmp/lapis-next-XXXXXX"));
    require(directory.isValid(), "fixture directory");
    const QDir root(directory.path());
    require(root.mkpath(QStringLiteral("bin")), "fixture bin");
    standIns(root);
    write(root.filePath(QStringLiteral("context.reply")),
          R"({"conversation": "c1", "turn": 7, "turns": [{"role": "agent", "text": "Done."}]})");
    write(root.filePath(QStringLiteral("predict.reply")),
          R"({"category": "approve", "candidates": [{"text": "go", "p": 0.8},
              {"text": "send", "p": 0.1}], "ms": 3100})");
    const auto log = root.filePath(QStringLiteral("logs/next_prompt.jsonl"));
    NextPrompt next(
        [](const QString& id) -> std::optional<NextPrompt::Agent> {
            if (id == QLatin1String("a"))
                return NextPrompt::Agent{{},
                                         QStringLiteral("~/x"),
                                         QStringLiteral("claude"),
                                         QStringLiteral("c1"),
                                         QStringLiteral("paste fix"),
                                         QStringLiteral("lapis"),
                                         QStringLiteral("> |")};
            if (id == QLatin1String("b"))
                return NextPrompt::Agent{QStringLiteral("devbox"),
                                         QStringLiteral("~/y"),
                                         QStringLiteral("codex"),
                                         {},
                                         QStringLiteral("gpu"),
                                         QStringLiteral("infra"),
                                         {}};
            if (id == QLatin1String("s"))
                return NextPrompt::Agent{{}, {}, QStringLiteral("shell"), {}, {}, {}, {}};
            return std::nullopt;
        },
        [] { return QJsonArray{QJsonObject{{QStringLiteral("title"), QStringLiteral("gpu")}}}; },
        [&root](const QString& name) { return fixtureProgram(root, name); }, {root.path(), log});
    int changes = 0;
    QObject::connect(&next, &NextPrompt::changed, [&changes] { ++changes; });
    const auto script = root.filePath(QStringLiteral("next_prompt.py"));
    require(read(script).contains("def predict("), "the helper is written beside the workspace");

    next.turnFinished(QStringLiteral("a"));
    QCoreApplication::processEvents();
    require(!QFileInfo::exists(root.filePath(QStringLiteral("context.args"))),
            "nothing runs while the setting is off");

    next.setSettings(on(60, QStringLiteral("low")));
    require(changes == 1 && next.enabled(), "turning it on is announced");
    changes = 0;
    next.turnFinished(QStringLiteral("s"));
    next.turnFinished(QStringLiteral("a"));
    require(waitFor([&] { return !next.suggestion(QStringLiteral("a")).isEmpty(); }),
            "a likely guess is offered");
    require(next.suggestion(QStringLiteral("a")) == QLatin1String("go") && changes == 1,
            "the most likely guess, once");
    const auto context = QString::fromUtf8(read(root.filePath(QStringLiteral("context.args"))))
                             .split(QLatin1Char('\n'));
    require(context.first() == script &&
                context.mid(1, 7) == QStringList{QStringLiteral("context"), QStringLiteral("--cli"),
                                                 QStringLiteral("claude"),
                                                 QStringLiteral("--folder"), QStringLiteral("~/x"),
                                                 QStringLiteral("--conversation"),
                                                 QStringLiteral("c1")},
            "the conversation is read on this Mac, by its id and folder");
    const auto predict = QString::fromUtf8(read(root.filePath(QStringLiteral("predict.args"))))
                             .split(QLatin1Char('\n'));
    require(predict.contains(QStringLiteral("--model")) &&
                predict.contains(QStringLiteral("claude-opus-5-5")) &&
                predict.contains(root.filePath(QStringLiteral("bin/claude"))) &&
                predict.contains(QStringLiteral("low")),
            "the model, the CLI and the effort");
    const auto bundle =
        QJsonDocument::fromJson(read(root.filePath(QStringLiteral("predict.stdin")))).object();
    const auto target =
        QDateTime::fromString(bundle.value(QStringLiteral("target")).toString(), Qt::ISODateWithMs);
    require(bundle.value(QStringLiteral("screen")).toString() == QLatin1String("> |") &&
                target.isValid() && target.timeSpec() == Qt::UTC &&
                bundle.value(QStringLiteral("agent")).toObject().value(QStringLiteral("title")) ==
                    QLatin1String("paste fix") &&
                bundle.value(QStringLiteral("agents")).toArray().size() == 1 &&
                bundle.value(QStringLiteral("context"))
                        .toObject()
                        .value(QStringLiteral("turns"))
                        .toArray()
                        .size() == 1,
            "the model sees the agent, its screen, the others and the conversation");

    auto logged = events(log);
    require(
        logged.size() == 1 &&
            logged[0].value(QStringLiteral("event")) == QLatin1String("predicted") &&
            logged[0].value(QStringLiteral("shown")).toBool() &&
            logged[0].value(QStringLiteral("turn")).toInt() == 7 &&
            logged[0].value(QStringLiteral("conversation")) == QLatin1String("c1") &&
            logged[0].value(QStringLiteral("candidates")).toArray().size() == 2 &&
            logged[0].value(QStringLiteral("offer")).toString().startsWith(QStringLiteral("a:")) &&
            logged[0].value(QStringLiteral("offer")).toString().endsWith(QStringLiteral(".1")) &&
            logged[0].value(QStringLiteral("v")).toInt() == 2,
        "the prediction is logged with where it belongs");
    const auto first_offer = logged[0].value(QStringLiteral("offer")).toString();
    require(next.enabled() && next.readyAgents() == QVariantMap{{QStringLiteral("a"), false}} &&
                next.offerKey(QStringLiteral("a")) == first_offer,
            "the agent is ready for Tab, its offer not yet seen");
    const auto others = QFile::ReadGroup | QFile::WriteGroup | QFile::ExeGroup | QFile::ReadOther |
                        QFile::WriteOther | QFile::ExeOther;
    require((QFileInfo(log).permissions() & others) == 0, "and only its owner can read the log");

    next.seenOffer({{QStringLiteral("session"), QStringLiteral("a")},
                    {QStringLiteral("offer"), QStringLiteral("stale")}});
    next.used(QStringLiteral("a"), true, 0, QStringLiteral("stale"));
    require(events(log).size() == 1 && !next.suggestion(QStringLiteral("a")).isEmpty(),
            "stale identities cannot mark or consume a newer offer");
    offerIsPublished(next, first_offer);
    require(next.readyAgents() == QVariantMap{{QStringLiteral("a"), true}}, "now seen");
    next.used(QStringLiteral("a"), false, 5);
    require(next.suggestion(QStringLiteral("a")).isEmpty() && next.readyAgents().isEmpty(),
            "a used suggestion is gone");
    logged = events(log);
    require(logged.size() == 3 &&
                logged[1].value(QStringLiteral("event")) == QLatin1String("seen") &&
                logged[1].value(QStringLiteral("offer")) == first_offer,
            "it was seen once");
    require(logged[2].value(QStringLiteral("event")) == QLatin1String("used") &&
                logged[2].value(QStringLiteral("offer")) == first_offer &&
                !logged[2].value(QStringLiteral("sent")).toBool() &&
                logged[2].value(QStringLiteral("typed_first")).toInt() == 5 &&
                logged[2].value(QStringLiteral("ms_after_seen")).toInteger() >= 0 &&
                logged[2].value(QStringLiteral("turn")).toInt() == 7,
            "and its use, with what was typed first, is logged");

    // Another machine: its conversation is read there, with the helper on stdin.
    write(root.filePath(QStringLiteral("predict.reply")),
          R"({"category": "new", "candidates": [{"text": "rerun it on 8 GPUs", "p": 0.2}]})");
    // Tie the offering behaviour to the parsed default, not only the struct.
    next.setSettings(lapis::desktop::parse_next_prompt(QJsonObject{
        {QStringLiteral("auto"), true}, {QStringLiteral("effort"), QStringLiteral("low")}}));
    next.turnFinished(QStringLiteral("b"));
    require(waitFor([&] { return events(log).size() == 4; }), "the remote prediction is logged");
    const auto ssh =
        QString::fromUtf8(read(root.filePath(QStringLiteral("ssh.args")))).split(QLatin1Char('\n'));
    require(
        ssh.contains(QStringLiteral("BatchMode=yes")) && ssh.contains(QStringLiteral("devbox")) &&
            ssh.at(ssh.size() - 2)
                .startsWith(QStringLiteral("python3 - 'context' '--cli' 'codex' '--folder' '~/y'")),
        "ssh runs the helper there without prompts");
    require(read(root.filePath(QStringLiteral("ssh.stdin"))).contains("def predict("),
            "and sends it on stdin");
    require(next.suggestion(QStringLiteral("b")) == QLatin1String("rerun it on 8 GPUs") &&
                events(log).last().value(QStringLiteral("shown")).toBool() &&
                !events(log).last().contains(QStringLiteral("min_confidence")) &&
                !events(log).last().contains(QStringLiteral("arm")),
            "an unlikely guess is still offered, with no floor or model arm recorded");

    // A new turn withdraws the old offer; the hourly cap stops more.
    write(root.filePath(QStringLiteral("predict.reply")),
          R"({"category": "status", "candidates": [{"text": "status?", "p": 0.9}]})");
    next.turnFinished(QStringLiteral("a"));
    require(
        waitFor([&] { return next.suggestion(QStringLiteral("a")) == QLatin1String("status?"); }),
        "offered again after the next turn");
    next.seen(QStringLiteral("a"));
    next.setSettings(on(3));
    next.turnFinished(QStringLiteral("a"));
    require(next.suggestion(QStringLiteral("a")).isEmpty(), "the next turn withdraws the offer");
    const auto tail = events(log);
    const auto& withdrawn = tail.at(tail.size() - 2);
    require(tail.last().value(QStringLiteral("event")) == QLatin1String("skipped") &&
                tail.last().value(QStringLiteral("reason")) == QLatin1String("hourly_cap"),
            "a turn past the hourly cap is recorded as skipped");
    require(withdrawn.value(QStringLiteral("event")) == QLatin1String("withdrawn") &&
                withdrawn.value(QStringLiteral("reason")) == QLatin1String("new_turn") &&
                withdrawn.value(QStringLiteral("seen")).toBool() &&
                withdrawn.value(QStringLiteral("offer")).toString().endsWith(QStringLiteral(".3")),
            "an offer seen but not used is recorded as replaced");
    QFile::remove(root.filePath(QStringLiteral("context.args")));
    static_cast<void>(waitFor([] { return false; }, 300));
    require(!QFileInfo::exists(root.filePath(QStringLiteral("context.args"))),
            "past the hourly cap nothing runs");
    // A prediction that fails is recorded with its stage and reason.
    const auto valid_context = read(root.filePath(QStringLiteral("context.reply")));
    write(root.filePath(QStringLiteral("context.reply")), R"({"error": "no transcript"})");
    next.setSettings(on(60));
    next.turnFinished(QStringLiteral("a"));
    require(waitFor([&] {
                return events(log).last().value(QStringLiteral("event")) == QLatin1String("failed");
            }),
            "a failed prediction is recorded");
    const auto failure = events(log).last();
    require(failure.value(QStringLiteral("stage")) == QLatin1String("context") &&
                failure.value(QStringLiteral("error")) == QLatin1String("no transcript") &&
                failure.value(QStringLiteral("cli")) == QLatin1String("claude"),
            "with where and why it failed");
    write(root.filePath(QStringLiteral("context.reply")),
          R"({"error": "private path and transcript must not become a log category"})");
    const auto before_private_failure = events(log).size();
    next.turnFinished(QStringLiteral("a"));
    require(waitFor([&] { return events(log).size() > before_private_failure; }),
            "arbitrary helper failures are recorded");
    require(events(log).last().value(QStringLiteral("error")) == QLatin1String("helper failed") &&
                !(read(log) + read(log + QStringLiteral(".1")))
                     .contains("private path and transcript must not become a log category"),
            "no field in any persisted record contains the private failure input");
    write(root.filePath(QStringLiteral("context.reply")), valid_context);
    // A model failure the helper classified keeps its cleaned line and kind,
    // and failures in a row show until a guess succeeds.
    const auto valid_predict = read(root.filePath(QStringLiteral("predict.reply")));
    write(root.filePath(QStringLiteral("predict.reply")),
          R"({"error": "Claude AI usage limit reached", "kind": "limit"})");
    const auto streak_before = next.failureStreak();
    for (int failure_index = 0; failure_index < 3; ++failure_index) {
        const auto before = events(log).size();
        next.turnFinished(QStringLiteral("a"));
        require(waitFor([&] { return events(log).size() > before; }), "the failure is logged");
    }
    require(events(log).last().value(QStringLiteral("kind")) == QLatin1String("limit") &&
                events(log).last().value(QStringLiteral("detail")) ==
                    QLatin1String("Claude AI usage limit reached"),
            "a classified failure keeps what it was");
    require(next.failureStreak() == streak_before + 3 &&
                next.lastFailure() == QLatin1String("a model usage limit"),
            "failures in a row are counted and named");
    write(root.filePath(QStringLiteral("predict.reply")), valid_predict);
    next.turnFinished(QStringLiteral("a"));
    require(waitFor([&] { return next.failureStreak() == 0; }), "a guess ends the streak");
    next.seen(QStringLiteral("a")); // so the next turn is not an unseen repeat
    next.setSettings(on(60));
    write(root.filePath(QStringLiteral("no-claude")), "yes");
    const auto before_missing_cli = events(log).size();
    next.turnFinished(QStringLiteral("a"));
    require(waitFor([&] {
                return events(log).size() > before_missing_cli &&
                       events(log).last().value(QStringLiteral("event")) == QLatin1String("failed");
            }),
            "missing prediction CLI recorded");
    require(events(log).last().value(QStringLiteral("error")) ==
                    QLatin1String("no Claude Code CLI on this Mac") &&
                events(log).last().value(QStringLiteral("stage")) == QLatin1String("predict"),
            "the missing Claude CLI retains its actionable category");
    QFile::remove(root.filePath(QStringLiteral("no-claude")));
    write(root.filePath(QStringLiteral("context.reply")),
          QByteArray(qsizetype{1024} * 1024 + 1, 'x'));
    const auto before_overflow = events(log).size();
    next.turnFinished(QStringLiteral("a"));
    require(waitFor([&] { return events(log).size() > before_overflow; }),
            "oversized helper output finishes with a bounded failure");
    require(events(log).last().value(QStringLiteral("error")) == QLatin1String("output too large"),
            "helper output is bounded before JSON parsing");
    write(log, QByteArray(qsizetype{4} * 1024 * 1024, '\n'));
    next.setSettings(on(0));
    next.turnFinished(QStringLiteral("a"));
    require(QFileInfo::exists(log + QStringLiteral(".1")) && QFileInfo(log).size() < 4096 &&
                events(log).size() == 1,
            "log rotation moves the full log aside and writes the new event to the active file");
    write(log, QByteArray(qsizetype{4} * 1024 * 1024, '\n'));
    next.turnFinished(QStringLiteral("a"));
    const auto archived = QFileInfo(log).dir().entryList(
        {QFileInfo(log).fileName() + QStringLiteral(".2*")}, QDir::Files);
    require(archived.size() == 1 && QFileInfo::exists(log + QStringLiteral(".1")),
            "a second rotation archives the first under its date instead of deleting it");
    next.setSettings({});
    require(!next.enabled(), "off");
    require(next.suggestion(QStringLiteral("a")).isEmpty(), "turning it off withdraws offers");
}
} // namespace

// What the person sent after an offer is recorded beside it once the
// conversation holds it: typed in by Tab and changed, their own words, or the
// guess unchanged. A turn the agent starts itself waits; /clear drops it.
// A turn with no new prompt since an unseen guess is a repeat: nothing is
// guessed until the agent is shown, and then once. The guess carries its
// probability; the live arms reach the helper and the log.
void repeatsWaitUntilShown() {
    QTemporaryDir directory(QStringLiteral("/tmp/lapis-next-XXXXXX"));
    require(directory.isValid(), "fixture directory");
    const QDir root(directory.path());
    require(root.mkpath(QStringLiteral("bin")), "fixture bin");
    standIns(root);
    write(root.filePath(QStringLiteral("context.reply")),
          R"({"conversation": "c1", "turn": 7, "turns": [{"role": "agent", "text": "Done."}]})");
    write(root.filePath(QStringLiteral("predict.reply")),
          R"({"candidates": [{"text": "go", "p": 0.6}]})");
    const auto log = root.filePath(QStringLiteral("logs/next_prompt.jsonl"));
    NextPrompt next(
        [](const QString&) -> std::optional<NextPrompt::Agent> {
            return NextPrompt::Agent{{},
                                     QStringLiteral("~/x"),
                                     QStringLiteral("claude"),
                                     QStringLiteral("c1"),
                                     QStringLiteral("paste fix"),
                                     QStringLiteral("lapis"),
                                     {}};
        },
        [] { return QJsonArray{}; },
        [&root](const QString& name) { return fixtureProgram(root, name); }, {root.path(), log});
    next.setSettings(on());
    next.turnFinished(QStringLiteral("a"));
    require(waitFor([&] { return !next.suggestion(QStringLiteral("a")).isEmpty(); }) &&
                next.confidence(QStringLiteral("a")) > 0.59,
            "a guess and its probability");
    next.turnFinished(QStringLiteral("a"));
    // This show races the context fetch: it arrives before the repeat callback
    // has recorded the deferral, and must still force the prediction.
    next.focused(QStringLiteral("a"));
    require(waitFor([&] { return !next.suggestion(QStringLiteral("a")).isEmpty(); }),
            "shown during the context fetch, the repeat is guessed");
    next.focused(QStringLiteral("a"));
    QCoreApplication::processEvents();
    int predicted = 0;
    for (const auto& event : events(log))
        if (event.value(QStringLiteral("event")) == QLatin1String("predicted"))
            ++predicted;
    require(predicted == 2, "once");

    // The main guess carries the provider chain to the helper.
    auto settings = on();
    settings.providers =
        QJsonArray{QJsonObject{{QStringLiteral("url"), QStringLiteral("https://one.test/v1")},
                               {QStringLiteral("model"), QStringLiteral("deepseek")},
                               {QStringLiteral("name"), QStringLiteral("one")}}};
    next.setSettings(settings);
    next.seen(QStringLiteral("a"));
    next.turnFinished(QStringLiteral("a"));
    require(waitFor([&] {
                const auto last = events(log).last();
                return last.value(QStringLiteral("event")) == QLatin1String("predicted");
            }),
            "a guess through the chain");
    {
        const auto arguments =
            QString::fromUtf8(read(root.filePath(QStringLiteral("predict.args"))))
                .split(QLatin1Char('\n'));
        const auto at = arguments.indexOf(QStringLiteral("--providers"));
        require(at > 0 && arguments.value(at + 1).contains(QLatin1String("https://one.test/v1")) &&
                    arguments.value(at + 1).contains(QLatin1String("\"model\":\"deepseek\"")),
                "the helper is given the provider chain");
    }

    // The context and prompt arms go to the helper and onto the guess.
    settings = on();
    settings.contextArms = {QStringLiteral("wide")};
    settings.promptArms = {QStringLiteral("checks")};
    next.setSettings(settings);
    {
        const auto before = events(log).size();
        next.seen(QStringLiteral("a"));
        next.turnFinished(QStringLiteral("a"));
        require(waitFor([&] {
                    const auto all = events(log);
                    return all.size() > before &&
                           all.last().value(QStringLiteral("event")) ==
                               QLatin1String("predicted") &&
                           all.last().value(QStringLiteral("context_arm")) == QLatin1String("wide");
                }),
                "a guess is logged with its context arm");
    }
    const auto arguments = QString::fromUtf8(read(root.filePath(QStringLiteral("predict.args"))))
                               .split(QLatin1Char('\n'));
    const auto at = arguments.indexOf(QStringLiteral("--context-arm"));
    require(at > 0 && arguments.value(at + 1) == QLatin1String("wide"),
            "the helper is told the context arm");
    const auto prompt_at = arguments.indexOf(QStringLiteral("--prompt-arm"));
    require(prompt_at > 0 && arguments.value(prompt_at + 1) == QLatin1String("checks"),
            "the helper is told the prompt arm");
    require(events(log).last().value(QStringLiteral("prompt_arm")) == QLatin1String("checks"),
            "a guess is logged with its prompt arm");

    // Coming back to an old guess: guessed again, and the return is logged.
    settings = on();
    settings.returnMinutes = 0;
    next.setSettings(settings);
    next.seen(QStringLiteral("a"));
    next.turnFinished(QStringLiteral("a"));
    require(waitFor([&] { return !next.suggestion(QStringLiteral("a")).isEmpty(); }),
            "a guess to come back to");
    next.focused(QStringLiteral("a"));
    require(waitFor([&] {
                const auto last = events(log).last();
                return last.value(QStringLiteral("event")) == QLatin1String("predicted") &&
                       last.value(QStringLiteral("returned_minutes")).toInt(-1) == 0;
            }),
            "a return guesses again, saying how long it has been");
    bool returned = false;
    bool withdrawn = false;
    for (const auto& event : events(log)) {
        returned |= event.value(QStringLiteral("event")) == QLatin1String("return") &&
                    event.value(QStringLiteral("age_minutes")).toInt(-1) == 0;
        withdrawn |= event.value(QStringLiteral("event")) == QLatin1String("withdrawn") &&
                     event.value(QStringLiteral("reason")) == QLatin1String("return");
    }
    require(returned && withdrawn, "the return and the replaced guess are logged");
    const auto stdin_bundle =
        QJsonDocument::fromJson(read(root.filePath(QStringLiteral("predict.stdin")))).object();
    require(stdin_bundle.value(QStringLiteral("returned_minutes")).toInt(-1) == 0,
            "the model is told the person came back");
    // Once per offer: the fresh guess is new, so a later look returns to it
    // again, but the same offer is never returned to twice.
    const auto count_returns = [&] {
        int count = 0;
        for (const auto& event : events(log))
            count += event.value(QStringLiteral("event")) == QLatin1String("return") ? 1 : 0;
        return count;
    };
    require(waitFor([&] { return !next.suggestion(QStringLiteral("a")).isEmpty(); }),
            "the fresh guess is offered");
    const auto before = count_returns();
    next.focused(QStringLiteral("a"));
    next.focused(QStringLiteral("a"));
    QCoreApplication::processEvents();
    require(count_returns() == before + 1, "each offer is returned to once");
}

// The chain answers the guess, an empty chain keeps the CLI route, and the
// chain works on a Mac without the Claude Code CLI.
void providerChainKeepsItsOwnRoute() {
    QTemporaryDir directory(QStringLiteral("/tmp/lapis-next-XXXXXX"));
    require(directory.isValid(), "fixture directory");
    const QDir root(directory.path());
    require(root.mkpath(QStringLiteral("bin")), "fixture bin");
    standIns(root);
    write(root.filePath(QStringLiteral("context.reply")),
          R"({"conversation": "c1", "turn": 7, "turns": [{"role": "agent", "text": "Done."}]})");
    write(root.filePath(QStringLiteral("predict.reply")),
          R"({"candidates": [{"text": "go", "p": 0.6}]})");
    const auto log = root.filePath(QStringLiteral("logs/next_prompt.jsonl"));
    NextPrompt next(
        [](const QString&) -> std::optional<NextPrompt::Agent> {
            return NextPrompt::Agent{{},
                                     QStringLiteral("~/x"),
                                     QStringLiteral("claude"),
                                     QStringLiteral("c1"),
                                     QStringLiteral("paste fix"),
                                     QStringLiteral("lapis"),
                                     {}};
        },
        [] { return QJsonArray{}; },
        [&root](const QString& name) { return fixtureProgram(root, name); }, {root.path(), log});
    const QJsonArray chain{
        QJsonObject{{QStringLiteral("url"), QStringLiteral("https://one.test/v1")},
                    {QStringLiteral("model"), QStringLiteral("deepseek")},
                    {QStringLiteral("name"), QStringLiteral("one")}}};
    const auto helper_arguments = [&root] {
        return QString::fromUtf8(read(root.filePath(QStringLiteral("predict.args"))))
            .split(QLatin1Char('\n'));
    };
    // The control guess carries the chain and the time budget it shares.
    auto settings = on();
    settings.providers = chain;
    next.setSettings(settings);
    const auto before_control = events(log).size();
    next.seen(QStringLiteral("a"));
    next.turnFinished(QStringLiteral("a"));
    require(waitFor([&] {
                const auto all = events(log);
                return all.size() > before_control &&
                       all.last().value(QStringLiteral("event")) == QLatin1String("predicted");
            }),
            "a guess through the chain");
    {
        const auto arguments = helper_arguments();
        const auto at = arguments.indexOf(QStringLiteral("--providers"));
        require(at > 0 && arguments.value(at + 1).contains(QLatin1String("https://one.test/v1")),
                "the control guess carries the chain");
        const auto budget = arguments.indexOf(QStringLiteral("--budget-seconds"));
        require(budget > 0 && arguments.value(budget + 1).toInt() > 0,
                "the chain is given the guess's time budget");
    }
    // Without a chain an empty list is passed, so the helper keeps the CLI
    // route rather than reading lapis.json itself.
    next.setSettings(on());
    const auto before_cli = events(log).size();
    next.seen(QStringLiteral("a"));
    next.turnFinished(QStringLiteral("a"));
    require(waitFor([&] {
                const auto all = events(log);
                return all.size() > before_cli &&
                       all.last().value(QStringLiteral("event")) == QLatin1String("predicted");
            }),
            "the CLI's guess is made");
    {
        const auto arguments = helper_arguments();
        const auto at = arguments.indexOf(QStringLiteral("--providers"));
        require(at > 0 && arguments.value(at + 1) == QLatin1String("[]") &&
                    !arguments.contains(QStringLiteral("--endpoint")),
                "no chain keeps the CLI route");
    }
    // The chain replaces the CLI call, so a Mac without the CLI still guesses.
    write(root.filePath(QStringLiteral("no-claude")), "yes");
    settings = on();
    settings.providers = chain;
    next.setSettings(settings);
    const auto without_cli = events(log).size();
    next.seen(QStringLiteral("a"));
    next.turnFinished(QStringLiteral("a"));
    require(waitFor([&] {
                const auto all = events(log);
                return all.size() > without_cli &&
                       all.last().value(QStringLiteral("event")) == QLatin1String("predicted");
            }),
            "the chain answers without the Claude Code CLI");
    for (const auto& event : events(log))
        require(event.value(QStringLiteral("error")).toString() !=
                    QLatin1String("no Claude Code CLI on this Mac"),
                "the chain does not need the CLI");
    QFile::remove(root.filePath(QStringLiteral("no-claude")));
}

void outcomesCompareWhatWasSent() {
    QTemporaryDir directory;
    const QDir root(directory.path());
    require(root.mkpath(QStringLiteral("bin")), "fixture bin");
    standIns(root);
    const auto log = root.filePath(QStringLiteral("next.jsonl"));
    NextPrompt next(
        [](const QString&) -> std::optional<NextPrompt::Agent> {
            NextPrompt::Agent agent;
            agent.cli = QStringLiteral("claude");
            return agent;
        },
        [] { return QJsonArray{}; },
        [&root](const QString& name) { return root.filePath(QStringLiteral("bin/") + name); },
        {root.path(), log});
    next.setSettings(on(60));
    const auto outcomes = [&log] {
        QList<QJsonObject> found;
        for (const auto& event : events(log))
            if (event.value(QStringLiteral("event")) == QLatin1String("outcome"))
                found << event;
        return found;
    };
    const auto turn = [&](const QByteArray& context, const QByteArray& guess) {
        write(root.filePath(QStringLiteral("context.reply")), context);
        write(root.filePath(QStringLiteral("predict.reply")),
              R"({"candidates":[{"text":")" + guess + R"(","p":0.3}]})");
        // Seen, so a turn without a new prompt is guessed again (an unseen
        // repeat waits for the agent to be shown; repeatsWaitUntilShown).
        if (!next.suggestion(QStringLiteral("a")).isEmpty())
            next.seen(QStringLiteral("a"));
        next.turnFinished(QStringLiteral("a"));
        require(waitFor([&] {
                    return next.suggestion(QStringLiteral("a")) == QString::fromUtf8(guess);
                }),
                "the guess is offered");
    };
    const auto answered = [&](int count) {
        return waitFor([&] { return outcomes().size() == count; });
    };

    turn(R"({"conversation":"c","turn":3})", "go now");
    require(!read(root.filePath(QStringLiteral("context.args"))).contains("--answered"),
            "nothing is asked for before an offer");
    const auto first = next.offerKey(QStringLiteral("a"));
    next.used(QStringLiteral("a"), false, 0, first);
    next.used(QStringLiteral("a"), true, 0, QStringLiteral("other"));
    require(events(log).last().value(QStringLiteral("sent")) != QJsonValue(true),
            "a second Tab for another offer is ignored");
    turn(R"({"conversation":"c","turn":4,"answered":{"turn":3,"text":"go now, and test it"}})",
         "status?");
    require(read(root.filePath(QStringLiteral("context.args"))).contains("--answered\n3\n"),
            "the offer's turn is asked for");
    require(answered(1), "an edited guess is recorded");
    auto outcome = outcomes().last();
    require(outcome.value(QStringLiteral("offer")).toString() == first &&
                outcome.value(QStringLiteral("result")) == QLatin1String("edited") &&
                outcome.value(QStringLiteral("filled")).toBool() &&
                outcome.value(QStringLiteral("sent_text")) ==
                    QLatin1String("go now, and test it") &&
                outcome.value(QStringLiteral("similarity")).toDouble() > 0.2 &&
                outcome.value(QStringLiteral("similarity")).toDouble() < 1.0,
            "typed in by Tab, then changed, with its similarity and the text sent");
    require(!outcome.value(QStringLiteral("tab_sent")).toBool(), "it was sent with Return");

    // The agent's own turn (no prompt sent yet) leaves the offer waiting.
    turn(R"({"conversation":"c","turn":4})", "status?");
    require(outcomes().size() == 1, "a turn without a prompt records nothing");
    turn(R"({"conversation":"c","turn":5,"answered":{"turn":4,"text":"what broke"}})", "go");
    require(answered(2), "their own prompt is recorded");
    outcome = outcomes().last();
    require(outcome.value(QStringLiteral("result")) == QLatin1String("own") &&
                !outcome.value(QStringLiteral("filled")).toBool(),
            "a prompt typed instead of the guess is their own");

    // A newer offer can replace a guess that is still waiting for the agent's
    // own turn. Taking the newer offer must re-anchor the outcome to it.
    const auto third = next.offerKey(QStringLiteral("a"));
    next.used(QStringLiteral("a"), false, 0, third);
    turn(R"({"conversation":"c","turn":5})", "go on");
    const auto replacement = next.offerKey(QStringLiteral("a"));
    require(replacement != third, "the agent's own turn did not offer a newer guess");
    next.seen(QStringLiteral("a"));
    next.used(QStringLiteral("a"), false, 0, replacement);
    next.used(QStringLiteral("a"), true, 0, replacement);
    require(events(log).last().value(QStringLiteral("event")) == QLatin1String("used") &&
                events(log).last().value(QStringLiteral("offer")) == replacement &&
                events(log).last().value(QStringLiteral("sent")).toBool() &&
                events(log).last().value(QStringLiteral("ms_after_seen")).toInteger() >= 0,
            "a second Tab's send is recorded with its wait");
    turn(R"({"conversation":"c","turn":6,"answered":{"turn":5,"text":"go on"}})", "again");
    require(answered(3) && outcomes().last().value(QStringLiteral("offer")) == replacement &&
                outcomes().last().value(QStringLiteral("result")) == QLatin1String("as_offered") &&
                outcomes().last().value(QStringLiteral("similarity")).toDouble() == 1.0 &&
                outcomes().last().value(QStringLiteral("tab_sent")).toBool(),
            "the newer guess sent by double Tab is the recorded outcome");

    // A new conversation (after /clear) drops the waiting offer.
    turn(R"({"conversation":"d","turn":0,"answered":{"turn":6,"text":"go on"}})", "go");
    QCoreApplication::processEvents();
    require(outcomes().size() == 3, "an offer from another conversation is not matched");

    // Similarity is bounded, while `as_offered` still requires the whole prompt
    // unchanged: a change beyond the score's bound is marked, not accepted.
    const auto long_guess = QByteArray(qsizetype{kComparedForTest + 1}, 'g');
    const auto long_context = QByteArrayLiteral(R"({"conversation":"c","turn":7})");
    turn(long_context, long_guess);
    next.used(QStringLiteral("a"), false, 0, next.offerKey(QStringLiteral("a")));
    const auto changed_after_bound = long_guess + 'x';
    turn(
        QByteArrayLiteral(R"json({"conversation":"c","turn":8,"answered":{"turn":7,"text":")json") +
            changed_after_bound + QByteArrayLiteral("\"}}"),
        "next");
    require(answered(4) &&
                outcomes().last().value(QStringLiteral("result")) == QLatin1String("edited") &&
                outcomes().last().value(QStringLiteral("similarity")).toDouble() == 1.0,
            "long prompts are classified and scored over the same bound");
    require(outcomes().last().value(QStringLiteral("similarity_bounded")).toBool(),
            "the outcome records when similarity ignored its suffix");

    // A much shorter, disjoint replacement gets the conservative shared-prefix
    // score of zero, rather than an inflated diagonal-band estimate.
    turn(R"({"conversation":"c","turn":9})", long_guess);
    next.used(QStringLiteral("a"), false, 0, next.offerKey(QStringLiteral("a")));
    const auto short_sent = QByteArray(qsizetype{400}, 'x');
    turn(QByteArrayLiteral(
             R"json({"conversation":"c","turn":10,"answered":{"turn":9,"text":")json") +
             short_sent + QByteArrayLiteral("\"}}"),
         "next");
    require(answered(5) &&
                outcomes().last().value(QStringLiteral("similarity")).toDouble() == 0.0 &&
                outcomes().last().value(QStringLiteral("similarity_bounded")).toBool(),
            "a disjoint shorter prompt is not given an inflated similarity");

    // A real shared prefix must contribute to the long-prompt fallback.
    turn(R"({"conversation":"c","turn":11})", long_guess);
    next.used(QStringLiteral("a"), false, 0, next.offerKey(QStringLiteral("a")));
    const auto shared_replacement = long_guess.left(1000) + QByteArray(qsizetype{300}, 'x');
    turn(QByteArrayLiteral(
             R"json({"conversation":"c","turn":12,"answered":{"turn":11,"text":")json") +
             shared_replacement + QByteArrayLiteral("\"}}"),
         "next");
    require(answered(6) &&
                outcomes().last().value(QStringLiteral("similarity")).toDouble() == 0.5 &&
                outcomes().last().value(QStringLiteral("similarity_bounded")).toBool(),
            "a long prompt's shared prefix contributes to bounded similarity");

    // The mirrored replacement pins the suffix half of the same fallback.
    turn(R"({"conversation":"c","turn":13})", long_guess);
    next.used(QStringLiteral("a"), false, 0, next.offerKey(QStringLiteral("a")));
    const auto tail_replacement = QByteArray(qsizetype{1000}, 'x') + long_guess.right(1000);
    turn(QByteArrayLiteral(
             R"json({"conversation":"c","turn":14,"answered":{"turn":13,"text":")json") +
             tail_replacement + QByteArrayLiteral("\"}}"),
         "next");
    require(answered(7) &&
                outcomes().last().value(QStringLiteral("similarity")).toDouble() == 0.5 &&
                outcomes().last().value(QStringLiteral("similarity_bounded")).toBool(),
            "a long prompt's shared suffix contributes to bounded similarity");
}

// The save and restore sides use the same durable bounds; an offer outside
// them is not written and then silently discarded on the next launch.
void savedEntriesRespectRestoreBounds() {
    QTemporaryDir directory;
    const QDir root(directory.path());
    require(root.mkpath(QStringLiteral("bin")), "fixture bin");
    standIns(root);
    const auto conversation = QString(201, QLatin1Char('c'));
    write(root.filePath(QStringLiteral("context.reply")),
          QStringLiteral(R"json({"conversation":"%1","turn":3})json").arg(conversation).toUtf8());
    write(root.filePath(QStringLiteral("predict.reply")),
          R"({"candidates":[{"text":"go on","p":0.8}]})");
    NextPrompt next(
        [&](const QString& id) -> std::optional<NextPrompt::Agent> {
            if (id != QLatin1String("a"))
                return std::nullopt;
            NextPrompt::Agent agent;
            agent.cli = QStringLiteral("claude");
            agent.folder = QStringLiteral("~/x");
            agent.conversation = conversation;
            agent.screen = QStringLiteral("> |");
            return agent;
        },
        [] { return QJsonArray{}; },
        [&root](const QString& name) { return root.filePath(QStringLiteral("bin/") + name); },
        NextPrompt::Files{root.path(), {}});
    next.setSettings(on(60));
    next.turnFinished(QStringLiteral("a"));
    require(waitFor([&] { return !next.suggestion(QStringLiteral("a")).isEmpty(); }),
            "an offer too long to save is still live");
    require(next.saveState().value(QStringLiteral("offers")).toObject().isEmpty(),
            "an over-limit conversation is not saved for a restore that would drop it");
}

// A restarted window brings back its offers, each shown again only once the
// conversation proves it current; the guesses whose outcome is still to be
// read; and a guess still being made. Nothing is shown or used for an agent
// that is gone, has moved to another conversation or started a new turn.
// The cases share one fixture and append-only log, so the sequential function
// intentionally remains one test case.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void restoredStateIsChecked() {
    QTemporaryDir directory;
    const QDir root(directory.path());
    require(root.mkpath(QStringLiteral("bin")), "fixture bin");
    standIns(root);
    const auto log = root.filePath(QStringLiteral("next.jsonl"));
    QHash<QString, QString> conversations{{QStringLiteral("a"), QStringLiteral("c")},
                                          {QStringLiteral("b"), QStringLiteral("c")},
                                          {QStringLiteral("c"), QStringLiteral("c")}};
    const auto lookup = [&conversations](const QString& id) -> std::optional<NextPrompt::Agent> {
        if (!conversations.contains(id))
            return std::nullopt;
        NextPrompt::Agent agent;
        agent.cli =
            id == QLatin1String("unsupported") ? QStringLiteral("kimi") : QStringLiteral("claude");
        agent.folder = QStringLiteral("~/x");
        agent.conversation = conversations.value(id);
        agent.screen = id == QLatin1String("unscreened") ? QString() : QStringLiteral("> |");
        return agent;
    };
    const auto make = [&] {
        auto next = std::make_unique<NextPrompt>(
            lookup, [] { return QJsonArray{}; },
            [&root](const QString& name) { return root.filePath(QStringLiteral("bin/") + name); },
            NextPrompt::Files{root.path(), log});
        next->setSettings(on(60));
        return next;
    };
    const auto withdrawn = [&log](const QString& key) {
        for (const auto& event : events(log))
            if (event.value(QStringLiteral("event")) == QLatin1String("withdrawn") &&
                event.value(QStringLiteral("offer")).toString() == key)
                return event.value(QStringLiteral("reason")).toString();
        return QString();
    };
    write(root.filePath(QStringLiteral("context.reply")), R"({"conversation":"c","turn":3})");
    write(root.filePath(QStringLiteral("predict.reply")),
          R"({"candidates":[{"text":"go on","p":0.8}]})");

    QJsonObject saved;
    QString key;
    QString used_key;
    {
        auto first = make();
        int state_changes = 0;
        QObject::connect(first.get(), &NextPrompt::stateChanged,
                         [&state_changes] { ++state_changes; });
        first->turnFinished(QStringLiteral("a"));
        first->turnFinished(QStringLiteral("b"));
        require(waitFor([&] {
                    return !first->suggestion(QStringLiteral("a")).isEmpty() &&
                           !first->suggestion(QStringLiteral("b")).isEmpty();
                }),
                "two guesses are offered");
        key = first->offerKey(QStringLiteral("a"));
        first->seen(QStringLiteral("a"));
        used_key = first->offerKey(QStringLiteral("b"));
        first->used(QStringLiteral("b"), false, 0, used_key);
        require(state_changes > 0, "every change asks for a save");
        saved = first->saveState();
    }
    const auto offer = saved.value(QStringLiteral("offers")).toObject().value(QStringLiteral("a"));
    require(offer.toObject().value(QStringLiteral("key")).toString() == key &&
                offer.toObject().value(QStringLiteral("turn")).toInt() == 3 &&
                offer.toObject().value(QStringLiteral("seenMs")).toDouble() > 0,
            "the offer is saved with its key, turn and when it was seen");
    require(saved.value(QStringLiteral("awaiting"))
                .toObject()
                .value(QStringLiteral("b"))
                .toObject()
                .value(QStringLiteral("filled"))
                .toBool(),
            "a guess Tab typed in waits for its outcome");
    // A guess still being made for "c" when the window went.
    saved.insert(QStringLiteral("owed"),
                 QJsonObject{{QStringLiteral("c"),
                              QJsonObject{{QStringLiteral("conversation"), QStringLiteral("c")}}}});

    // The same turn: the offer comes back as it was.
    {
        QFile::remove(root.filePath(QStringLiteral("predict.args")));
        write(root.filePath(QStringLiteral("predict.reply")),
              R"({"candidates":[{"text":"carry on","p":0.8}]})");
        auto next = make();
        next->restoreState(saved);
        require(next->suggestion(QStringLiteral("a")).isEmpty(),
                "nothing shows before its conversation is read");
        require(waitFor([&] { return next->suggestion(QStringLiteral("a")) == "go on"; }),
                "an offer still current comes back");
        require(next->offerKey(QStringLiteral("a")) == key &&
                    next->readyAgents().value(QStringLiteral("a")).toBool(),
                "with its key, already seen");
        require(waitFor([&] { return next->suggestion(QStringLiteral("c")) == "carry on"; }),
                "a guess still owed is made again");
        next->used(QStringLiteral("a"), false, 2, key);
        require(events(log).last().value(QStringLiteral("event")) == QLatin1String("used") &&
                    events(log).last().value(QStringLiteral("offer")).toString() == key,
                "Tab uses the restored offer under its key");
        // The prompt sent after the guess Tab typed before the restart.
        write(root.filePath(QStringLiteral("context.reply")),
              R"({"conversation":"c","turn":4,"answered":{"turn":3,"text":"go on"}})");
        next->turnFinished(QStringLiteral("b"));
        require(waitFor([&] {
                    for (const auto& event : events(log))
                        if (event.value(QStringLiteral("event")) == QLatin1String("outcome") &&
                            event.value(QStringLiteral("offer")).toString() == used_key)
                            return event.value(QStringLiteral("result")) ==
                                       QLatin1String("as_offered") &&
                                   event.value(QStringLiteral("filled")).toBool();
                    return false;
                }),
                "the outcome of a guess typed before the restart is recorded");
        require(waitFor([&] { return !next->suggestion(QStringLiteral("b")).isEmpty(); }),
                "and the next guess follows");
    }

    // The agent started a new turn while no window watched.
    {
        write(root.filePath(QStringLiteral("context.reply")), R"({"conversation":"c","turn":4})");
        auto restored = saved;
        restored.remove(QStringLiteral("owed"));
        auto next = make();
        next->restoreState(restored);
        require(waitFor([&] { return withdrawn(key) == QLatin1String("new_turn"); }),
                "a stale offer is dropped and recorded");
        require(next->suggestion(QStringLiteral("a")).isEmpty() && next->readyAgents().isEmpty(),
                "and never shown");
    }

    // After /clear (another conversation), and for an agent that is gone.
    QFile::remove(log);
    {
        conversations.insert(QStringLiteral("a"), QStringLiteral("d"));
        conversations.remove(QStringLiteral("b"));
        conversations.remove(QStringLiteral("c"));
        conversations.insert(QStringLiteral("unsupported"), QStringLiteral("c"));
        QFile::remove(root.filePath(QStringLiteral("context.args")));
        auto next = make();
        const auto unsupported_key = QStringLiteral("unsupported:%1.1").arg(key.section(':', 1));
        auto with_unsupported = saved;
        with_unsupported.insert(
            QStringLiteral("offers"),
            QJsonObject{{QStringLiteral("unsupported"),
                         QJsonObject{{QStringLiteral("key"), unsupported_key},
                                     {QStringLiteral("text"), QStringLiteral("x")},
                                     {QStringLiteral("conversation"), QStringLiteral("c")},
                                     {QStringLiteral("turn"), 3},
                                     {QStringLiteral("seenMs"), 0}}}});
        next->restoreState(saved);
        next->restoreState(with_unsupported);
        QCoreApplication::processEvents();
        require(withdrawn(key) == QLatin1String("moved"), "another conversation drops the offer");
        require(withdrawn(unsupported_key) == QLatin1String("unsupported"),
                "a CLI lapis cannot guess for is named in the durable log");
        require(next->suggestion(QStringLiteral("a")).isEmpty() &&
                    next->saveState().value(QStringLiteral("awaiting")).toObject().isEmpty() &&
                    next->saveState().value(QStringLiteral("owed")).toObject().isEmpty(),
                "nothing comes back for a gone agent or another conversation");
        require(!QFileInfo::exists(root.filePath(QStringLiteral("context.args"))),
                "without reading any conversation");
    }

    // Malformed entries and the setting off restore nothing.
    const auto malformed = [&] {
        {
            conversations.insert(QStringLiteral("a"), QStringLiteral("c"));
            auto next = make();
            int pruning_changes = 0;
            QObject::connect(next.get(), &NextPrompt::stateChanged,
                             [&pruning_changes] { ++pruning_changes; });
            next->restoreState(QJsonObject{
                {QStringLiteral("offers"),
                 QJsonObject{{QStringLiteral("a"), QJsonObject{{QStringLiteral("key"), "other:1"},
                                                               {QStringLiteral("text"), "x"},
                                                               {QStringLiteral("turn"), 1},
                                                               {QStringLiteral("seenMs"), 0}}}}},
                {QStringLiteral("awaiting"), QJsonArray{}}});
            QCoreApplication::processEvents();
            require(pruning_changes > 0, "a rejected saved entry still asks for one new snapshot");
            require(next->saveState().value(QStringLiteral("offers")).toObject().isEmpty(),
                    "an offer whose key is not the agent's is ignored");
        }
    };
    malformed();

    // A screen that has not reattached does not become a context-only guess.
    const auto unscreened = [&] {
        {
            conversations.insert(QStringLiteral("unscreened"), QStringLiteral("c"));
            auto next = make();
            next->setOwedRetryForTesting(1, 1);
            next->restoreState(QJsonObject{
                {QStringLiteral("owed"), QJsonObject{{QStringLiteral("unscreened"),
                                                      QJsonObject{{QStringLiteral("conversation"),
                                                                   QStringLiteral("c")}}}}}});
            QFile::remove(root.filePath(QStringLiteral("predict.args")));
            QElapsedTimer wait;
            wait.start();
            while (wait.elapsed() < 50)
                QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
            require(next->suggestion(QStringLiteral("unscreened")).isEmpty() &&
                        !next->saveState()
                             .value(QStringLiteral("owed"))
                             .toObject()
                             .value(QStringLiteral("unscreened"))
                             .toObject()
                             .isEmpty(),
                    "a guess with no reattached screen stays owed");
            require(QFileInfo(root.filePath(QStringLiteral("predict.args"))).exists() == false,
                    "and does not call the model");
            next->setSettings({});
            next->restoreState(saved);
            QCoreApplication::processEvents();
            require(next->saveState().value(QStringLiteral("offers")).toObject().isEmpty(),
                    "nothing comes back while the setting is off");
        }
    };
    unscreened();
}

void ompOffersRestoreBySessionId() {
    QTemporaryDir directory;
    const QDir root(directory.path());
    require(root.mkpath(QStringLiteral("bin")), "fixture bin");
    standIns(root);
    write(root.filePath(QStringLiteral("context.reply")),
          R"({"conversation":"2026-09-28T10-00-00-000Z_omp1",
              "conversation_id":"omp1","turn":3})");
    write(root.filePath(QStringLiteral("predict.reply")),
          R"({"candidates":[{"text":"check the disk","p":0.8}]})");
    const auto lookup = [](const QString& id) -> std::optional<NextPrompt::Agent> {
        if (id != QLatin1String("a"))
            return std::nullopt;
        NextPrompt::Agent agent;
        agent.cli = QStringLiteral("omp");
        agent.folder = QStringLiteral("~/x");
        agent.conversation = QStringLiteral("omp1");
        agent.screen = QStringLiteral("> |");
        return agent;
    };
    const auto make = [&] {
        auto next = std::make_unique<NextPrompt>(
            lookup, [] { return QJsonArray{}; },
            [&root](const QString& name) { return root.filePath(QStringLiteral("bin/") + name); },
            NextPrompt::Files{root.path(), root.path() + QStringLiteral("/next.jsonl")});
        next->setSettings(on(60));
        return next;
    };
    QJsonObject saved;
    {
        auto first = make();
        first->turnFinished(QStringLiteral("a"));
        require(waitFor([&] { return !first->suggestion(QStringLiteral("a")).isEmpty(); }),
                "an OMP guess is offered");
        saved = first->saveState();
    }
    const auto offer =
        saved.value(QStringLiteral("offers")).toObject().value(QStringLiteral("a")).toObject();
    require(offer.value(QStringLiteral("conversation")).toString() ==
                    QLatin1String("2026-09-28T10-00-00-000Z_omp1") &&
                offer.value(QStringLiteral("conversationId")).toString() == QLatin1String("omp1"),
            "both OMP identities are saved");
    auto restored = make();
    restored->restoreState(saved);
    require(waitFor([&] {
                return restored->suggestion(QStringLiteral("a")) == QLatin1String("check the disk");
            }),
            "an OMP offer restores by the CLI session id");
}

// Folder walls (`separate`, `off`), all under a made-up temporary root: work/client
// is walled, work/clientele only starts like it, dev/app is outside, private is off.
void wallSettingsAndZones() {
    const auto none = lapis::desktop::parse_next_prompt(QJsonValue());
    require(none.separate.isEmpty() && none.off.isEmpty() && !none.walled(),
            "no folders are walled by default");
    const auto listed = lapis::desktop::parse_next_prompt(
        QJsonDocument::fromJson(R"({"separate": ["~/work/client", " /srv/a ", "relative", 3, "",
                                                 "~other/x"],
                                    "off": "not a list"})")
            .object());
    require(listed.separate == QStringList({QStringLiteral("~/work/client"),
                                            QStringLiteral("/srv/a")}) &&
                listed.off.isEmpty() && listed.walled(),
            "absolute and ~ folders are kept, trimmed; anything else is dropped");

    QTemporaryDir directory;
    require(directory.isValid(), "fixture directory");
    const QDir root(directory.path());
    for (const auto* folder : {"work/client/sub", "work/clientele", "dev/app", "private"})
        require(root.mkpath(QLatin1String(folder)), "fixture folders");
    require(QFile::link(root.filePath(QStringLiteral("work/client")),
                        root.filePath(QStringLiteral("alias"))),
            "a link into the walled folder");
    NextPromptSettings settings;
    settings.separate = {root.filePath(QStringLiteral("work")),
                         root.filePath(QStringLiteral("work/client"))};
    settings.off = {root.filePath(QStringLiteral("private"))};
    const auto zone = [&settings, &root](const QString& machine, const QString& folder) {
        return lapis::desktop::folder_zone(
            settings, machine, folder.startsWith(QLatin1Char('/')) ? folder : root.filePath(folder));
    };
    require(zone({}, QStringLiteral("work/client/sub")) == QLatin1String("s1") &&
                zone({}, QStringLiteral("work/clientele")) == QLatin1String("s0") &&
                zone({}, QStringLiteral("alias/new")) == QLatin1String("s1") &&
                zone({}, QStringLiteral("dev/app")) == QLatin1String("") &&
                zone({}, QStringLiteral("private/x")) == QLatin1String("o0"),
            "here: the deepest listed folder, by component, through links, off first");
    require(lapis::desktop::folder_zone(settings, {}, {}) == QLatin1String("?") &&
                lapis::desktop::folder_zone(settings, {}, QStringLiteral("rel/x")) ==
                    QLatin1String("?"),
            "an unknown folder here is unknown");
    require(lapis::desktop::folder_zone(NextPromptSettings{}, {}, {}).isEmpty(),
            "without walls every folder is outside");

    NextPromptSettings remote;
    remote.separate = {QStringLiteral("~/work/client"), QStringLiteral("~/a b")};
    const auto there = [&remote](const QString& folder) {
        return lapis::desktop::folder_zone(remote, QStringLiteral("box"), folder);
    };
    require(there(QStringLiteral("~/work/client/r")) == QLatin1String("s0") &&
                there(QStringLiteral("~/work/clientele")) == QLatin1String("") &&
                there(QStringLiteral("~/'a b'/c")) == QLatin1String("s1") &&
                there(QStringLiteral("~/x/../work/client")) == QLatin1String("s0") &&
                there(QStringLiteral("~/../elsewhere")) == QLatin1String("?") &&
                there(QStringLiteral("/home/u/work/client")) == QLatin1String("?") &&
                there(QStringLiteral("~/$(x)")) == QLatin1String("?") &&
                there({}) == QLatin1String("?"),
            "another machine: compared as written, ~ only with ~");
}

void foldersAreWalled() {
    QTemporaryDir directory;
    require(directory.isValid(), "fixture directory");
    const QDir root(directory.path());
    require(root.mkpath(QStringLiteral("bin")), "fixture bin");
    for (const auto* folder : {"work/client/sub", "work/clientele", "dev/app", "private"})
        require(root.mkpath(QLatin1String(folder)), "fixture folders");
    standIns(root);
    write(root.filePath(QStringLiteral("predict.reply")),
          R"({"candidates":[{"text":"go","p":0.8}]})");
    const auto log = root.filePath(QStringLiteral("next.jsonl"));
    const QHash<QString, QString> folders{
        {QStringLiteral("out"), root.filePath(QStringLiteral("dev/app"))},
        {QStringLiteral("in"), root.filePath(QStringLiteral("work/client/sub"))},
        {QStringLiteral("pre"), root.filePath(QStringLiteral("work/clientele"))},
        {QStringLiteral("off"), root.filePath(QStringLiteral("private"))},
        {QStringLiteral("unknown"), QString()}};
    NextPrompt next(
        [&folders](const QString& id) -> std::optional<NextPrompt::Agent> {
            NextPrompt::Agent agent;
            agent.cli = QStringLiteral("claude");
            agent.folder = folders.value(id);
            agent.title = id;
            agent.conversation = id;
            agent.screen = QStringLiteral("> ");
            return agent;
        },
        [&folders] {
            QJsonArray agents;
            for (const auto& id : {"out", "in", "pre", "off", "unknown"})
                agents.append(QJsonObject{{QStringLiteral("title"), QLatin1String(id)},
                                          {QStringLiteral("category"), QStringLiteral("c")},
                                          {QStringLiteral("status"), QStringLiteral("Done")},
                                          {QStringLiteral("waiting"), false},
                                          {QStringLiteral("machine"), QString()},
                                          {QStringLiteral("folder"), folders.value(id)}});
            agents.append(QJsonObject{{QStringLiteral("title"), QStringLiteral("far")},
                                      {QStringLiteral("machine"), QStringLiteral("box")},
                                      {QStringLiteral("folder"), QStringLiteral("~/work/client")}});
            return agents;
        },
        [&root](const QString& name) { return root.filePath(QStringLiteral("bin/") + name); },
        {root.path(), log});
    // One guess for `id` with the helper reporting `reply` as its context:
    // the titles of the other agents the model was given, and its arguments.
    struct Guess {
        QStringList titles;
        QStringList context;
        bool predicted{};
    };
    // `wall` is the zone the helper reports, or absent when null. Each guess
    // is a new turn, so none is held back as an unseen repeat.
    int turn = 0;
    const auto guess = [&](const QString& id, const char* wall) {
        QFile::remove(root.filePath(QStringLiteral("predict.stdin")));
        QFile::remove(root.filePath(QStringLiteral("context.args")));
        QJsonObject reply{{QStringLiteral("conversation"), id}, {QStringLiteral("turn"), ++turn}};
        if (wall != nullptr)
            reply.insert(QStringLiteral("wall"), QLatin1String(wall));
        write(root.filePath(QStringLiteral("context.reply")), QJsonDocument(reply).toJson());
        const auto before = events(log).size();
        next.turnFinished(id);
        // The previous offer's withdrawal is logged first; wait for the end.
        const auto ended = [&] {
            const auto logged = events(log);
            if (logged.size() <= before)
                return false;
            const auto event = logged.last().value(QStringLiteral("event")).toString();
            return event == QLatin1String("predicted") || event == QLatin1String("skipped") ||
                   event == QLatin1String("failed");
        };
        require(waitFor(ended), "the guess is logged");
        Guess made;
        made.predicted = events(log).last().value(QStringLiteral("event")) ==
                         QLatin1String("predicted");
        const auto bundle =
            QJsonDocument::fromJson(read(root.filePath(QStringLiteral("predict.stdin")))).object();
        for (const auto& agent : bundle.value(QStringLiteral("agents")).toArray()) {
            const auto object = agent.toObject();
            require(!object.contains(QStringLiteral("machine")) &&
                        !object.contains(QStringLiteral("folder")),
                    "where an agent runs decides the wall and is not sent");
            made.titles << object.value(QStringLiteral("title")).toString();
        }
        made.context = QString::fromUtf8(read(root.filePath(QStringLiteral("context.args"))))
                           .split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        return made;
    };

    // No lists: every agent, and the helper's arguments as before.
    next.setSettings(on());
    const auto open = guess(QStringLiteral("out"), nullptr);
    require(open.predicted && open.titles == QStringList({QStringLiteral("out"),
                                                          QStringLiteral("in"),
                                                          QStringLiteral("pre"),
                                                          QStringLiteral("off"),
                                                          QStringLiteral("unknown"),
                                                          QStringLiteral("far")}),
            "without walls every agent is shown");
    require(!open.context.contains(QStringLiteral("--walls")), "and no walls are passed");

    auto settings = on();
    settings.separate = {root.filePath(QStringLiteral("work/client"))};
    settings.off = {root.filePath(QStringLiteral("private"))};
    next.setSettings(settings);
    const auto outside = guess(QStringLiteral("out"), "");
    require(outside.predicted &&
                outside.titles == QStringList({QStringLiteral("out"), QStringLiteral("pre")}),
            "outside: no walled, off, unknown or uncomparable agent");
    const auto walls = outside.context.indexOf(QStringLiteral("--walls"));
    require(walls > 0 &&
                QJsonDocument::fromJson(outside.context.value(walls + 1).toUtf8()).object() ==
                    QJsonObject{{QStringLiteral("separate"),
                                 QJsonArray{root.filePath(QStringLiteral("work/client"))}},
                                {QStringLiteral("off"),
                                 QJsonArray{root.filePath(QStringLiteral("private"))}}},
            "the helper is given the lists to resolve where it runs");
    const auto inside = guess(QStringLiteral("in"), "s0");
    require(inside.predicted && inside.titles == QStringList{QStringLiteral("in")},
            "inside: only the agents inside the same folder");
    // Where lapis started it and what its conversation records disagree, or
    // the helper does not say: unknown, which an off list leaves unguessed.
    require(!guess(QStringLiteral("out"), "s0").predicted &&
                !guess(QStringLiteral("out"), nullptr).predicted,
            "with an off list, an agent in an unknown zone is not guessed");

    // Off: not read at all when lapis knows, not predicted when the helper says.
    const auto off = guess(QStringLiteral("off"), "o0");
    require(!off.predicted && off.context.isEmpty() &&
                events(log).last().value(QStringLiteral("reason")) ==
                    QLatin1String("off_folder"),
            "an agent in an off folder is not even read");
    const auto moved = guess(QStringLiteral("out"), "o0");
    require(!moved.predicted && !moved.context.isEmpty() &&
                events(log).last().value(QStringLiteral("reason")) ==
                    QLatin1String("off_folder"),
            "a conversation recorded in an off folder is read, never predicted");
    const auto unknown = guess(QStringLiteral("unknown"), "?");
    require(!unknown.predicted, "with an off list, an unknown folder is not guessed");
    settings.off.clear();
    next.setSettings(settings);
    const auto unknown_separate = guess(QStringLiteral("unknown"), "?");
    require(unknown_separate.predicted && unknown_separate.titles.isEmpty(),
            "with only separate folders, an unknown one is guessed alone");
    const auto disagree = guess(QStringLiteral("out"), "s0");
    require(disagree.predicted && disagree.titles.isEmpty(),
            "a conversation recorded elsewhere than lapis started it sees no other agent");
    const auto silent = guess(QStringLiteral("out"), nullptr);
    require(silent.predicted && silent.titles.isEmpty(),
            "a helper that does not say where the conversation is: no other agent");
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        settingsReadFromTheConfig();
        wallSettingsAndZones();
        foldersAreWalled();
        screensReadAsText();
        concurrentContextsRespectCap();
        missingProbabilityIsNotOffered();
        predictsAndOffers();
        repeatsWaitUntilShown();
        providerChainKeepsItsOwnRoute();
        outcomesCompareWhatWasSent();
        savedEntriesRespectRestoreBounds();
        restoredStateIsChecked();
        ompOffersRestoreBySessionId();
    } catch (const std::exception& error) {
        std::cerr << "next_prompt_test: " << error.what() << '\n';
        return 1;
    }
    std::cout << "next_prompt_test: PASS\n";
    return 0;
}
