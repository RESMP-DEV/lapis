// PlanSignIn adds a Claude Code plan from inside lapis: the helper runs a
// stand-in `claude setup-token` that "opens" its link and then prints a
// token. The link is copied, the token lands owner-only under the plan's
// name, and lapis.json gains the plan with this Mac among its machines.
#include "keymap.hpp"
#include "plan_sign_in.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <functional>
#include <iostream>
#include <stdexcept>

using lapis::desktop::KeyMap;
using lapis::desktop::PlanSignIn;

namespace {
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
bool waitFor(const std::function<bool()>& done, int ms = 15000) {
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
    require(file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
                file.write(text) == text.size(),
            "write a fixture file");
}
QByteArray token() { return "sk-ant-oat01-" + QByteArray(40, 'B'); }
QString link() {
    return QStringLiteral("https://claude.com/cai/oauth/authorize?code=true&state=s");
}

// The existing plan with an email gains this Mac; a new email is a new plan.
void plansRecordThisMac() {
    QTemporaryDir directory;
    require(directory.isValid(), "fixture directory");
    const QDir root(directory.path());
    const auto config = root.filePath(QStringLiteral("lapis.json"));
    write(config, R"({"version": 1, "accounts": {"claude": [
        {"name": "work", "email": "someone@example.com", "home": "devbox"}]}})");
    KeyMap keymap;
    keymap.setSourcePathForTesting(config);
    require(keymap.load(), "config loads");
    const auto add = [&keymap](const QString& email) {
        return keymap.addPlanMachine(
            {.cli = QStringLiteral("claude"), .email = email, .machine = {}});
    };
    require(add(QStringLiteral("Someone@Example.com")) == QLatin1String("work"),
            "a plan with that email keeps its name");
    require(add(QStringLiteral("new@place.dev")) == QLatin1String("new-place"),
            "a new email is named from itself");
    const auto plans = QJsonDocument::fromJson(read(config))
                           .object()
                           .value(QStringLiteral("accounts"))
                           .toObject()
                           .value(QStringLiteral("claude"))
                           .toArray();
    require(plans.size() == 2 &&
                plans.at(0).toObject().value(QStringLiteral("home")).toString() ==
                    QLatin1String("devbox") &&
                plans.at(0).toObject().value(QStringLiteral("machines")).toArray() ==
                    QJsonArray{QStringLiteral("local")} &&
                plans.at(1).toObject().value(QStringLiteral("email")).toString() ==
                    QLatin1String("new@place.dev"),
            "both are written, usable on this Mac");
    const auto& accounts = keymap.accounts().accounts;
    require(accounts.size() == 2 && accounts.front().machines.contains(QString()),
            "and read back at once");
}

void signsInAnyAccount() {
    QTemporaryDir directory;
    require(directory.isValid(), "fixture directory");
    const QDir root(directory.path());
    const auto claude = root.filePath(QStringLiteral("claude"));
    write(claude, "#!/bin/sh\nopen '" + link().toUtf8() + "'\nsleep 1\nprintf 'Your token:\\r\\n" +
                      token() + "\\r\\n'\nsleep 30\n");
    QFile::setPermissions(claude, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    QStringList copied;
    QStringList recorded;
    PlanSignIn signIn(
        [&claude](const QString& name) {
            return name == QLatin1String("claude") ? claude : QStandardPaths::findExecutable(name);
        },
        [&copied](const QString& text) { copied << text; },
        [&recorded](const QString& email, QString*) {
            recorded << email;
            return QStringLiteral("work");
        },
        {.helper = root.filePath(QStringLiteral("runtime/plan_sign_in.py")),
         .accounts = root.filePath(QStringLiteral("accounts"))});
    signIn.start();
    require(waitFor([&] { return signIn.state() == QLatin1String("waiting"); }),
            "the link comes back");
    require(signIn.link() == link() && copied == QStringList{link()}, "and is copied");
    signIn.setEmail(QStringLiteral(" Someone@Example.com "));
    require(waitFor([&] { return signIn.state() == QLatin1String("done"); }),
            "signing in with an email already given records the plan");
    const auto kept = root.filePath(QStringLiteral("accounts/claude/work.token"));
    require(read(kept) == token() + '\n' &&
                QFileInfo(kept).permissions() ==
                    (QFile::ReadOwner | QFile::WriteOwner | QFile::ReadUser | QFile::WriteUser),
            "the token is kept owner-only under the plan's name");
    require(recorded == QStringList{QStringLiteral("someone@example.com")} &&
                signIn.plan() == QLatin1String("work"),
            "for the email given");
    require(!QFileInfo::exists(root.filePath(QStringLiteral("accounts/claude/.signing-in.token"))),
            "nothing is left pending");

    // A sign-in that is abandoned leaves no token.
    signIn.start();
    require(waitFor([&] { return signIn.state() == QLatin1String("waiting"); }), "a second link");
    signIn.cancel();
    require(signIn.state() == QLatin1String("idle"), "cancel ends it");
    require(waitFor([&] {
                return !QFileInfo::exists(
                    root.filePath(QStringLiteral("accounts/claude/.signing-in.token")));
            }),
            "without keeping a token");

    // Claude Code missing is said plainly.
    PlanSignIn missing([](const QString&) { return QString(); }, [](const QString&) {},
                       [](const QString&, QString*) { return QString(); },
                       {.helper = root.filePath(QStringLiteral("runtime/x.py")),
                        .accounts = root.filePath(QStringLiteral("accounts"))});
    missing.start();
    require(missing.state() == QLatin1String("failed") &&
                missing.message().contains(QStringLiteral("not installed")),
            "a missing Claude Code fails with a reason");
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        plansRecordThisMac();
        signsInAnyAccount();
    } catch (const std::exception& error) {
        std::cerr << "plan_sign_in_test: " << error.what() << '\n';
        return 1;
    }
    std::cout << "plan_sign_in_test: PASS\n";
    return 0;
}
