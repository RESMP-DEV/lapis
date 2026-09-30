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
#include <QPointer>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>

#include <algorithm>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <sys/stat.h>

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
            {.cli = QStringLiteral("claude"), .email = email, .machine = {}, .expectedName = {}});
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
    QString claude = root.filePath(QStringLiteral("claude"));
    write(claude, "#!/bin/sh\nopen '" + link().toUtf8() + "'\nsleep 1\nprintf 'Your token:\\r\\n" +
                      token() + "\\r\\n'\nsleep 30\n");
    QFile::setPermissions(claude, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    // A stand-in ssh: "devbox" keeps what arrives on stdin; "gone" is down.
    QString ssh = root.filePath(QStringLiteral("ssh"));
    write(ssh, R"(#!/usr/bin/env python3
import pathlib
import shlex
import subprocess
import sys
root = pathlib.Path(__file__).resolve().parent
host, command = sys.argv[-2:]
if host == "gone":
    print("fixture connection refused", file=sys.stderr)
    raise SystemExit(255)
data = sys.stdin.buffer.read()
if host == "devbox":
    (root / "ssh.args").write_text("\n".join(sys.argv[1:]))
    (root / "ssh.stdin").write_bytes(data)
remote = root / "remote" / host
remote.mkdir(parents=True, exist_ok=True)
if host == "short":
    previous = remote / "accounts/claude/work.token"
    previous.parent.mkdir(parents=True, exist_ok=True)
    previous.write_bytes(b"previous token")
    data = data[:-1]
# Only the destination prefix is redirected; run the actual emitted command.
command = command.replace("~/.lapis", shlex.quote(str(remote)))
command = command.replace("$HOME/.lapis", str(remote))
raise SystemExit(subprocess.run(["/bin/sh", "-c", command], input=data).returncode)
)");
    QFile::setPermissions(ssh, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    QStringList copied;
    QStringList opened;
    QStringList recorded;
    PlanSignIn signIn(
        {.program =
             [&claude, &ssh](const QString& name) {
                 if (name == QLatin1String("claude"))
                     return claude;
                 return name == QLatin1String("ssh") ? ssh : QStandardPaths::findExecutable(name);
             },
         .copy = [&copied](const QString& text) { copied << text; },
         .open = [&opened](const QString& text) { opened << text; },
         .record =
             [&recorded](const QString& email, const QString& machine, const QString& expected,
                         const PlanSignIn::Prepare& prepare, QString* reason) {
                 require(expected == (machine.isEmpty() ? QString() : QStringLiteral("work")),
                         "copy completion must carry the captured plan identity");
                 if (prepare && !prepare(QStringLiteral("work"), reason))
                     return QString();
                 recorded << email + QLatin1Char('@') +
                                 (machine.isEmpty() ? QStringLiteral("mac") : machine);
                 return QStringLiteral("work");
             },
         .machines =
             [](const QString&) {
                 return QStringList{QStringLiteral("devbox"), QStringLiteral("gone"),
                                    QStringLiteral("short")};
             }},
        {.helper = root.filePath(QStringLiteral("runtime/plan_sign_in.py")),
         .accounts = root.filePath(QStringLiteral("accounts"))});
    signIn.start();
    require(waitFor([&] { return signIn.state() == QLatin1String("waiting"); }),
            "the link comes back");
    require(signIn.link() == link() && copied == QStringList{link()} &&
                opened == QStringList{link()},
            "and is opened in the browser and copied");
    signIn.openLink();
    require(opened.size() == 2, "and can be opened again");
    signIn.setEmail(QStringLiteral(" Someone@Example.com "));
    require(waitFor([&] { return signIn.state() == QLatin1String("done"); }),
            "signing in with an email already given records the plan");
    const auto kept = root.filePath(QStringLiteral("accounts/claude/work.token"));
    require(read(kept) == token() + '\n' &&
                QFileInfo(kept).permissions() ==
                    (QFile::ReadOwner | QFile::WriteOwner | QFile::ReadUser | QFile::WriteUser),
            "the token is kept owner-only under the plan's name");
    require(waitFor([&] { return !signIn.spreading(); }), "the copies to other machines end");
    require(recorded == QStringList({QStringLiteral("someone@example.com@mac"),
                                     QStringLiteral("someone@example.com@devbox")}) &&
                signIn.plan() == QLatin1String("work") &&
                signIn.machines() ==
                    QStringList({QStringLiteral("this Mac"), QStringLiteral("devbox")}),
            "for the email given, here and on the machine that took it");
    require(read(root.filePath(QStringLiteral("ssh.stdin"))) == token() + '\n' &&
                !read(root.filePath(QStringLiteral("ssh.args"))).contains(token()) &&
                read(root.filePath(QStringLiteral("ssh.args")))
                    .contains("mktemp ~/.lapis/accounts/claude/.work.XXXXXX") &&
                read(root.filePath(QStringLiteral("ssh.args")))
                    .contains("mv -f \"$tmp\" "
                              "\"$HOME/.lapis/accounts/claude/work.token\""),
            "the token goes on stdin into a remote temporary file, then is moved whole");
    require(signIn.message().contains(QStringLiteral("Not reachable: gone")),
            "a machine that did not take it is named");
    require(QDir(root.filePath(QStringLiteral("accounts/claude")))
                .entryList({QStringLiteral(".signing-in-*.token")}, QDir::Files | QDir::Hidden)
                .isEmpty(),
            "nothing is left pending");

    // A second attempt has no identity left over. The token may arrive before
    // the person submits the email; only an email submitted to this attempt
    // can finish it.
    recorded.clear();
    require(read(root.filePath(QStringLiteral("remote/short/accounts/claude/work.token"))) ==
                "previous token",
            "a short remote transfer preserves its previous credential");
    require(QDir(root.filePath(QStringLiteral("remote/short/accounts/claude")))
                .entryList({QStringLiteral(".work.*")}, QDir::Files | QDir::Hidden)
                .isEmpty(),
            "failed remote copy removes its private temporary file");
    signIn.start();
    require(waitFor([&] { return signIn.state() == QLatin1String("waiting"); }),
            "the stale state is cleared and a new link appears");
    require(waitFor([&] { return signIn.state() == QLatin1String("signedIn"); }),
            "a signed-in token waits for this attempt's email");
    require(recorded.isEmpty() && signIn.plan().isEmpty(),
            "the previous email is not reused for the new token");
    signIn.setEmail(QStringLiteral("second@place.dev"));
    require(waitFor([&] { return signIn.state() == QLatin1String("done"); }),
            "the newly submitted email finishes the attempt");
    require(recorded == QStringList{QStringLiteral("second@place.dev@mac")},
            "the new attempt records only its own email");
    signIn.cancel();
    require(signIn.state() == QLatin1String("idle"), "a completed attempt can be closed");

    // A sign-in that is abandoned leaves no token.
    signIn.start();
    require(waitFor([&] { return signIn.state() == QLatin1String("waiting"); }), "a second link");
    signIn.cancel();
    require(signIn.state() == QLatin1String("idle"), "cancel ends it");
    require(waitFor([&] {
                return QDir(root.filePath(QStringLiteral("accounts/claude")))
                    .entryList({QStringLiteral(".signing-in-*.token")}, QDir::Files | QDir::Hidden)
                    .isEmpty();
            }),
            "without keeping a token");

    // Claude Code missing is said plainly.
    PlanSignIn missing({.program = [](const QString&) { return QString(); },
                        .copy = [](const QString&) {},
                        .open = [](const QString&) {},
                        .record = [](const QString&, const QString&, const QString&,
                                     const PlanSignIn::Prepare&, QString*) { return QString(); },
                        .machines = [](const QString&) { return QStringList(); }},
                       {.helper = root.filePath(QStringLiteral("runtime/x.py")),
                        .accounts = root.filePath(QStringLiteral("accounts"))});
    missing.start();
    require(missing.state() == QLatin1String("failed") &&
                missing.message().contains(QStringLiteral("not installed")),
            "a missing Claude Code fails with a reason");
}

// A replacement is committed whole, and an unsafe recorder cannot use the
// sign-in flow to put a token at a shell-controlled path.
void discardsUnregisteredTokenOnDestruction() {
    QTemporaryDir directory;
    require(directory.isValid(), "private unregistered-token fixture");
    const QDir root(directory.path());
    auto command = root.filePath(QStringLiteral("command"));
    write(command, "#!/bin/sh\nmkdir -p \"$(dirname \"$3\")\"\nprintf '" + token() +
                       "\n' > \"$3\"\nprintf '%s\n' '{\"signedIn\": true}'\n");
    QFile::setPermissions(command, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    const auto pending = [&] {
        return QDir(root.filePath(QStringLiteral("accounts/claude")))
            .entryList({QStringLiteral(".signing-in-*.token")}, QDir::Files | QDir::Hidden);
    };
    {
        PlanSignIn signIn({.program = [command](const QString&) { return command; },
                           .copy = [](const QString&) {},
                           .open = [](const QString&) {},
                           .record = [](const QString&, const QString&, const QString&,
                                        const PlanSignIn::Prepare&, QString*) { return QString(); },
                           .machines = [](const QString&) { return QStringList{}; }},
                          {.helper = root.filePath(QStringLiteral("runtime/helper.py")),
                           .accounts = root.filePath(QStringLiteral("accounts"))});
        signIn.start();
        require(waitFor([&] { return signIn.state() == QLatin1String("signedIn"); }) &&
                    pending().size() == 1,
                "received token remains private while this attempt waits for an email");
        require(waitFor([&] {
                    const auto processes = signIn.findChildren<QProcess*>();
                    return std::all_of(processes.cbegin(), processes.cend(), [](const QProcess* p) {
                        return p->state() == QProcess::NotRunning;
                    });
                }),
                "helper has exited before the abandoned controller is destroyed");
    }
    require(pending().isEmpty(), "normal destruction removes an unregistered staging credential");
}

void keepsTokensWholeAndNamedSafely() {
    QTemporaryDir directory;
    require(directory.isValid(), "fixture directory");
    const QDir root(directory.path());
    QString claude = root.filePath(QStringLiteral("claude"));
    write(claude, "#!/bin/sh\nopen '" + link().toUtf8() +
                      "'\n"
                      "sleep 1\nprintf 'Your token:\\r\\n" +
                      token() + "\\r\\n'\nsleep 30\n");
    QFile::setPermissions(claude, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    const auto replace = [&](const QString& recordedName, bool directoryWritable) {
        const QDir accounts(root.filePath(QStringLiteral("accounts")));
        require(QDir().mkpath(accounts.filePath(QStringLiteral("claude"))),
                "fixture account folder");
        const auto existing = accounts.filePath(QStringLiteral("claude/work.token"));
        write(existing, "old-" + token());
        if (!directoryWritable)
            QFile::setPermissions(accounts.filePath(QStringLiteral("claude")),
                                  QFile::ReadOwner | QFile::ExeOwner);
        PlanSignIn signIn({.program =
                               [&claude](const QString& name) {
                                   if (name == QLatin1String("claude"))
                                       return claude;
                                   return QStandardPaths::findExecutable(name);
                               },
                           .copy = [](const QString&) {},
                           .open = [](const QString&) {},
                           .record =
                               [recordedName](const QString&, const QString&, const QString&,
                                              const PlanSignIn::Prepare& prepare, QString* reason) {
                                   return prepare && !prepare(recordedName, reason) ? QString()
                                                                                    : recordedName;
                               },
                           .machines = [](const QString&) { return QStringList(); }},
                          {.helper = root.filePath(QStringLiteral("runtime/plan_sign_in.py")),
                           .accounts = accounts.absolutePath()});
        signIn.start();
        require(waitFor([&] { return signIn.state() == QLatin1String("signedIn"); }),
                "the replacement fixture produces its staging token");
        // The helper restores its folder while writing the pending token; take
        // that permission away only after signedIn to isolate token commit.
        if (!directoryWritable)
            ::chmod(QFile::encodeName(accounts.filePath(QStringLiteral("claude"))).constData(),
                    0500);
        signIn.setEmail(QStringLiteral("Someone@Example.com"));
        require(waitFor([&] {
                    return signIn.state() == QLatin1String("done") ||
                           signIn.state() == QLatin1String("failed");
                }),
                "the replacement attempt ends");
        if (!directoryWritable)
            ::chmod(QFile::encodeName(accounts.filePath(QStringLiteral("claude"))).constData(),
                    0700);
        return std::pair{signIn.state(), read(existing)};
    };
    const auto replaced = replace(QStringLiteral("work"), true);
    require(replaced.first == QLatin1String("done") && replaced.second == token() + '\n',
            "an existing token is replaced whole");
    require(QDir(root.filePath(QStringLiteral("accounts/claude")))
                .entryList({QStringLiteral(".signing-in-*.token")}, QDir::Files | QDir::Hidden)
                .isEmpty(),
            "the pending copy is retired after the replacement");

    const auto failed = replace(QStringLiteral("work"), false);
    require(failed.first == QLatin1String("failed") && failed.second == "old-" + token(),
            "a failed replacement leaves the old credential in place");
}

void rejectsMalformedHelperOutputAndUnsafeNames() {
    QTemporaryDir directory;
    require(directory.isValid(), "fixture directory");
    const QDir root(directory.path());
    auto command = root.filePath(QStringLiteral("command"));
    auto helper = root.filePath(QStringLiteral("runtime/plan_sign_in.py"));
    PlanSignIn signIn(
        {.program =
             [&command, &helper](const QString& name) {
                 if (name == QLatin1String("python3"))
                     return command;
                 if (name == QLatin1String("claude"))
                     return helper;
                 return QString();
             },
         .copy = [](const QString&) {},
         .open = [](const QString&) {},
         .record =
             [](const QString&, const QString&, const QString&, const PlanSignIn::Prepare& prepare,
                QString* reason) {
                 const auto name = QStringLiteral("bad;path");
                 return prepare && !prepare(name, reason) ? QString() : name;
             },
         .machines = [](const QString&) { return QStringList{QStringLiteral("devbox")}; }},
        {.helper = helper, .accounts = root.filePath(QStringLiteral("accounts"))});
    write(command, "#!/bin/sh\nshift 3\nprintf '%s\\n' "
                   "'{\"signedIn\": true}'\n");
    QFile::setPermissions(command, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    signIn.start();
    require(waitFor([&] { return signIn.state() == QLatin1String("signedIn"); }),
            "signed-in JSON is accepted");
    signIn.setEmail(QStringLiteral("someone@example.com"));
    require(waitFor([&] { return signIn.state() == QLatin1String("failed"); }),
            "a missing or malformed token fails instead of spreading");
    require(signIn.message().contains(QStringLiteral("usable token")), "the failure says why");

    write(command, "#!/bin/sh\nmkdir -p \"$(dirname \"$3\")\"\nprintf '" + token() +
                       "\n' > \"$3\"\nprintf '%s\n' '{\"signedIn\": true}'\n");
    QFile::setPermissions(command, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    signIn.start();
    require(waitFor([&] { return signIn.state() == QLatin1String("signedIn"); }),
            "the second malformed-name fixture starts");
    signIn.setEmail(QStringLiteral("someone@example.com"));
    require(waitFor([&] { return signIn.state() == QLatin1String("failed"); }),
            "an unsafe recorded name never reaches a command or a path");
    require(!QFileInfo::exists(root.filePath(QStringLiteral("ssh.args"))),
            "no remote command was built");
    require(!QFileInfo::exists(root.filePath(QStringLiteral("accounts/claude/bad;path.token"))),
            "and no unsafe token path was used");
}

// A prior copy is killed and forgets its captured identity before a new
// attempt's state is installed.
void stopsOldCopiesBeforeANewAttempt() {
    QTemporaryDir directory;
    require(directory.isValid(), "fixture directory");
    const QDir root(directory.path());
    QString claude = root.filePath(QStringLiteral("claude"));
    write(claude, "#!/bin/sh\nopen '" + link().toUtf8() +
                      "'\n"
                      "sleep 1\nprintf 'Your token:\\r\\n" +
                      token() + "\\r\\n'\nsleep 30\n");
    QFile::setPermissions(claude, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    QString ssh = root.filePath(QStringLiteral("ssh"));
    write(ssh, "#!/bin/sh\nsleep 5\ncat >/dev/null\n");
    QFile::setPermissions(ssh, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    QStringList recorded;
    PlanSignIn signIn(
        {.program =
             [&claude, &ssh](const QString& name) {
                 if (name == QLatin1String("claude"))
                     return claude;
                 if (name == QLatin1String("ssh"))
                     return ssh;
                 return QStandardPaths::findExecutable(name);
             },
         .copy = [](const QString&) {},
         .open = [](const QString&) {},
         .record =
             [&recorded](const QString& email, const QString& machine, const QString& expected,
                         const PlanSignIn::Prepare& prepare, QString* reason) {
                 require(expected == (machine.isEmpty() ? QString() : QStringLiteral("work")),
                         "copy completion must carry the captured plan identity");
                 if (prepare && !prepare(QStringLiteral("work"), reason))
                     return QString();
                 recorded << email + QLatin1Char('@') +
                                 (machine.isEmpty() ? QStringLiteral("mac") : machine);
                 return QStringLiteral("work");
             },
         .machines = [](const QString&) { return QStringList{QStringLiteral("devbox")}; }},
        {.helper = root.filePath(QStringLiteral("runtime/plan_sign_in.py")),
         .accounts = root.filePath(QStringLiteral("accounts"))});
    signIn.start();
    require(waitFor([&] { return signIn.state() == QLatin1String("waiting"); }), "the link");
    signIn.setEmail(QStringLiteral("first@example.com"));
    require(waitFor([&] { return signIn.state() == QLatin1String("done"); }), "the first plan");
    require(signIn.spreading(), "the slow old copy is still in flight");
    QPointer<QProcess> old_copy;
    for (auto* process : signIn.findChildren<QProcess*>())
        if (process->program() == ssh)
            old_copy = process;
    require(old_copy, "the old copy process is observable");
    signIn.start();
    require(!signIn.spreading(), "a new attempt stops the old token's copies");
    require(waitFor([&] { return signIn.state() == QLatin1String("signedIn"); }), "the new token");
    require(recorded == QStringList{QStringLiteral("first@example.com@mac")},
            "the stopped copy records nothing else");
    require(waitFor([&] {
                QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
                return old_copy.isNull();
            }),
            "the old copy guard completed and released its process");
    require(recorded == QStringList{QStringLiteral("first@example.com@mac")},
            "the killed copy cannot adopt the new attempt");
    signIn.cancel();
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        plansRecordThisMac();
        signsInAnyAccount();
        keepsTokensWholeAndNamedSafely();
        discardsUnregisteredTokenOnDestruction();
        rejectsMalformedHelperOutputAndUnsafeNames();
        stopsOldCopiesBeforeANewAttempt();
    } catch (const std::exception& error) {
        std::cerr << "plan_sign_in_test: " << error.what() << '\n';
        return 1;
    }
    std::cout << "plan_sign_in_test: PASS\n";
    return 0;
}
