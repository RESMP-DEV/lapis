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
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QVariantMap>
#include <lapis/session/terminal.hpp>

#include <functional>
#include <iostream>
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
                defaults.model == QLatin1String("claude-opus-5-5") &&
                defaults.minConfidence == 0.0 && defaults.maxPerHour == 60,
            "off by default, Opus, every guess offered, sixty an hour");
    const auto set = lapis::desktop::parse_next_prompt(
        QJsonDocument::fromJson(R"({"auto": true, "model": "claude-sonnet-5", "effort": "low",
                                    "minConfidence": 7, "maxPerHour": -3})")
            .object());
    require(set.automatic && set.model == QLatin1String("claude-sonnet-5") &&
                set.effort == QLatin1String("low") && set.minConfidence == 1.0 &&
                set.maxPerHour == 0,
            "each field, clamped");
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
    require(bundle.value(QStringLiteral("screen")).toString() == QLatin1String("> |") &&
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
    next.seen(QStringLiteral("a"));
    next.seen(QStringLiteral("a"));
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
    auto cautious = on(60, QStringLiteral("low"));
    cautious.minConfidence = 0.4;
    next.setSettings(cautious);
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
    require(next.suggestion(QStringLiteral("b")).isEmpty() &&
                !events(log).last().value(QStringLiteral("shown")).toBool(),
            "below a chosen minConfidence, a guess is logged but not offered");
    // Tie the offering behaviour to the parsed default, not only the struct.
    const auto default_on = lapis::desktop::parse_next_prompt(QJsonObject{
        {QStringLiteral("auto"), true}, {QStringLiteral("effort"), QStringLiteral("low")}});
    next.setSettings(default_on);
    next.turnFinished(QStringLiteral("b"));
    require(waitFor([&] {
                return next.suggestion(QStringLiteral("b")) == QLatin1String("rerun it on 8 GPUs");
            }),
            "an unlikely guess is still offered by default");

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
                withdrawn.value(QStringLiteral("offer")).toString().endsWith(QStringLiteral(".4")),
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
    write(root.filePath(QStringLiteral("no-claude")), "yes");
    const auto before_missing_cli = events(log).size();
    next.turnFinished(QStringLiteral("a"));
    require(waitFor([&] { return events(log).size() > before_missing_cli; }),
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
            "log rotation retains one backup and writes the new event to the active file");
    next.setSettings({});
    require(!next.enabled(), "off");
    require(next.suggestion(QStringLiteral("a")).isEmpty(), "turning it off withdraws offers");
}
} // namespace

// What the person sent after an offer is recorded beside it once the
// conversation holds it: typed in by Tab and changed, their own words, or the
// guess unchanged. A turn the agent starts itself waits; /clear drops it.
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
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        settingsReadFromTheConfig();
        screensReadAsText();
        concurrentContextsRespectCap();
        missingProbabilityIsNotOffered();
        predictsAndOffers();
        outcomesCompareWhatWasSent();
    } catch (const std::exception& error) {
        std::cerr << "next_prompt_test: " << error.what() << '\n';
        return 1;
    }
    std::cout << "next_prompt_test: PASS\n";
    return 0;
}
