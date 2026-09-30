// LimitResets runs the reset helper on each machine with Claude Code or Codex
// agents: here on this machine and on "devbox" through stand-ins for python3
// and ssh that record what they were given and print a canned report.
#include "limit_resets.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTimer>

#include <iostream>
#include <stdexcept>
#include <vector>

using lapis::desktop::LimitResets;
using lapis::desktop::LimitResetSettings;

namespace {
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
void standIn(const QDir& root, const QString& name, const QString& record) {
    const auto path = root.filePath(QStringLiteral("bin/") + name);
    // The arguments land last and whole, so a test that sees them sees stdin too.
    write(path, QStringLiteral("#!/bin/sh\nprintf '%s\\n' \"$@\" > '%1.part'\ncat > '%1.stdin'\n"
                               "mv '%1.part' '%1.args'\ncat '%1.reply'\n")
                    .arg(root.filePath(record))
                    .toUtf8());
    require(QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner),
            "make a stand-in executable");
}

struct Spent {
    QString machine, cli, email, title, why;
};

void sweepsEachMachineWithItsOwnSignIn() {
    QTemporaryDir directory(QStringLiteral("/tmp/lapis-resets-XXXXXX"));
    require(directory.isValid(), "fixture directory");
    const QDir root(directory.path());
    require(root.mkpath(QStringLiteral("bin")), "fixture bin");
    standIn(root, QStringLiteral("python3"), QStringLiteral("local"));
    standIn(root, QStringLiteral("ssh"), QStringLiteral("remote"));
    write(root.filePath(QStringLiteral("local.reply")),
          R"({"accounts": [{"cli": "claude", "email": "someone@example.com", "decision": "restore",
              "attempt": "block|k", "result": "reset", "credit": {"title": "Launch reset"},
              "confirmed": true}, {"cli": "codex", "error": "no Codex sign-in"}]})");
    write(root.filePath(QStringLiteral("remote.reply")),
          R"({"accounts": [{"cli": "codex", "decision": "not-needed"}]})");
    const QByteArray stored = "{\"claudeAiOauth\":\n {\"accessToken\": \"t\"}}\n";
    LimitResets resets(
        [] { return QStringList{QString(), QStringLiteral("devbox")}; },
        [](const QString& id) {
            if (id == QLatin1String("a"))
                return std::pair{QString(), QStringLiteral("claude")};
            if (id == QLatin1String("b"))
                return std::pair{QStringLiteral("devbox"), QStringLiteral("codex")};
            return std::pair{QString(), QStringLiteral("shell")};
        },
        [&root](const QString& id) { return root.filePath(QStringLiteral("bin/") + id); },
        [stored] { return stored; }, root.path());
    resets.setSettings(
        {.automatic = true, .minBlockedMinutes = 30, .keepCredits = 1, .salvageHours = 6});
    std::vector<Spent> spent;
    std::vector<QString> declined;
    QObject::connect(&resets, &LimitResets::spent,
                     [&spent](const QString& machine, const QString& cli, const QString& email,
                              const QString& title, const QString& why) {
                         spent.push_back({machine, cli, email, title, why});
                     });
    QObject::connect(&resets, &LimitResets::declined,
                     [&declined](const QString&, const QString&, const QString& reason) {
                         declined.push_back(reason);
                     });
    const auto script = root.filePath(QStringLiteral("limit_resets.py"));
    require(read(script).contains("def plan("), "the helper is written beside the workspace");
    const auto others = QFile::ReadGroup | QFile::WriteGroup | QFile::ExeGroup | QFile::ReadOther |
                        QFile::WriteOther | QFile::ExeOther;
    require((QFileInfo(script).permissions() & others) == 0, "and only its owner can read it");

    resets.sweep();
    require(waitFor([&] {
                return spent.size() == 1 &&
                       QFileInfo::exists(root.filePath(QStringLiteral("remote.args")));
            }),
            "both machines answer");
    const auto local =
        QString::fromUtf8(read(root.filePath(QStringLiteral("local.args")))).split('\n');
    require(local.first() == script, "this Mac runs the helper file");
    for (const auto* word : {"--apply", "--min-blocked-minutes", "30", "--keep", "1",
                             "--salvage-hours", "6", "--claude-credentials-stdin"})
        require(local.contains(QString::fromLatin1(word)),
                "with the settings and its credentials flag");
    require(read(root.filePath(QStringLiteral("local.stdin"))) == stored.simplified() + '\n',
            "the keychain's sign-in goes on stdin as one line");
    const auto remote =
        QString::fromUtf8(read(root.filePath(QStringLiteral("remote.args")))).split('\n');
    for (const auto* word : {"BatchMode=yes", "ControlPath=none", "-T", "devbox"})
        require(remote.contains(QString::fromLatin1(word)),
                "ssh runs without prompts on its own connection");
    require(remote.at(remote.size() - 2).startsWith(QStringLiteral("python3 - '--apply'")) &&
                !remote.at(remote.size() - 2).contains(QStringLiteral("credentials")),
            "another machine reads its own sign-in");
    require(read(root.filePath(QStringLiteral("remote.stdin"))).contains("def plan("),
            "and gets the helper on stdin");
    const auto& first = spent.front();
    require(first.machine.isEmpty() && first.cli == QLatin1String("claude") &&
                first.email == QLatin1String("someone@example.com") &&
                first.title == QLatin1String("Launch reset") &&
                first.why == QLatin1String("blocked"),
            "a spent reset is reported with its account and reason");
    require(declined.empty(), "nothing asked for, nothing declined");

    QFile::remove(root.filePath(QStringLiteral("local.args")));
    resets.sweep();
    require(
        waitFor([&] { return QFileInfo::exists(root.filePath(QStringLiteral("local.args"))); }) &&
            waitFor([&] { return spent.size() == 2; }),
        "a second sweep runs");
    const auto again =
        QString::fromUtf8(read(root.filePath(QStringLiteral("local.args")))).split('\n');
    require(again.contains(QStringLiteral("--attempted")) &&
                again.contains(QStringLiteral("block|k")),
            "an attempted reset is not tried again");

    write(root.filePath(QStringLiteral("local.reply")),
          R"({"accounts": [{"cli": "claude", "decision": "no-credit"}]})");
    require(resets.canUseNow(QStringLiteral("a")) && resets.canUseNow(QStringLiteral("b")) &&
                !resets.canUseNow(QStringLiteral("s")),
            "only Claude Code and Codex agents have resets");
    resets.setSettings({.automatic = false});
    resets.useNow(QStringLiteral("a"));
    require(waitFor([&] { return declined.size() == 1; }), "an unavailable reset is declined");
    require(declined.front() == QLatin1String("no-credit"), "with the helper's reason");
    const auto asked =
        QString::fromUtf8(read(root.filePath(QStringLiteral("local.args")))).split('\n');
    require(asked.contains(QStringLiteral("--now")) && asked.contains(QStringLiteral("claude")) &&
                !asked.contains(QStringLiteral("--apply")),
            "asking spends only that CLI's reset, with automatic spending off");
}

void settingsReadFromTheConfig() {
    const auto defaults = lapis::desktop::parse_limit_resets(QJsonValue());
    require(defaults == LimitResetSettings{}, "OMP's defaults without a setting");
    require(defaults.automatic && defaults.minBlockedMinutes == 60 && defaults.keepCredits == 0 &&
                defaults.salvageHours == 12,
            "on, an hour, no reserve, twelve hours");
    const auto set = lapis::desktop::parse_limit_resets(
        QJsonDocument::fromJson(
            R"({"auto": false, "minBlockedMinutes": 15, "keepCredits": -2, "salvageHours": 24})")
            .object());
    require(!set.automatic && set.minBlockedMinutes == 15 && set.keepCredits == 0 &&
                set.salvageHours == 24,
            "each field, with negatives clamped");
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        settingsReadFromTheConfig();
        sweepsEachMachineWithItsOwnSignIn();
    } catch (const std::exception& error) {
        std::cerr << "limit_resets_test: " << error.what() << '\n';
        return 1;
    }
    std::cout << "limit_resets_test: PASS\n";
    return 0;
}
