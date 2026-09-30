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

void settingsReadFromTheConfig() {
    const auto defaults = lapis::desktop::parse_next_prompt(QJsonValue());
    require(defaults == NextPromptSettings{} && !defaults.automatic &&
                defaults.model == QLatin1String("claude-opus-5-5") &&
                defaults.minConfidence == 0.4 && defaults.maxPerHour == 60,
            "off by default, Opus, 0.4, sixty an hour");
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
        [&root](const QString& name) { return root.filePath(QStringLiteral("bin/") + name); },
        {root.path(), log});
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

    next.seen(QStringLiteral("a"));
    next.seen(QStringLiteral("a"));
    require(next.readyAgents() == QVariantMap{{QStringLiteral("a"), true}}, "now seen");
    next.used(QStringLiteral("a"), true, 5);
    require(next.suggestion(QStringLiteral("a")).isEmpty() && next.readyAgents().isEmpty(),
            "a used suggestion is gone");
    logged = events(log);
    require(logged.size() == 3 &&
                logged[1].value(QStringLiteral("event")) == QLatin1String("seen") &&
                logged[1].value(QStringLiteral("offer")) == first_offer,
            "it was seen once");
    require(logged[2].value(QStringLiteral("event")) == QLatin1String("used") &&
                logged[2].value(QStringLiteral("offer")) == first_offer &&
                logged[2].value(QStringLiteral("sent")).toBool() &&
                logged[2].value(QStringLiteral("typed_first")).toInt() == 5 &&
                logged[2].value(QStringLiteral("ms_after_seen")).toInteger() >= 0 &&
                logged[2].value(QStringLiteral("turn")).toInt() == 7,
            "and its use, with what was typed first, is logged");

    // Another machine: its conversation is read there, with the helper on stdin.
    write(root.filePath(QStringLiteral("predict.reply")),
          R"({"category": "new", "candidates": [{"text": "rerun it on 8 GPUs", "p": 0.2}]})");
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
            "an unlikely guess is logged but not offered");

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
    const auto withdrawn = tail.at(tail.size() - 2);
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
    next.setSettings({});
    require(!next.enabled(), "off");
    require(next.suggestion(QStringLiteral("a")).isEmpty(), "turning it off withdraws offers");
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        settingsReadFromTheConfig();
        screensReadAsText();
        predictsAndOffers();
    } catch (const std::exception& error) {
        std::cerr << "next_prompt_test: " << error.what() << '\n';
        return 1;
    }
    std::cout << "next_prompt_test: PASS\n";
    return 0;
}
