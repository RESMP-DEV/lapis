#include "limit_resets.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QTemporaryDir>

#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>

using lapis::desktop::LimitResets;
namespace {
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
bool waitFor(const std::function<bool()>& done) {
    QElapsedTimer clock;
    clock.start();
    while (!done() && clock.elapsed() < 10000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return done();
}
QByteArray read(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
void write(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    require(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "write fixture file");
    require(file.write(bytes) == bytes.size(), "write complete fixture");
}

// The helper is a real subprocess, but it only reads/writes this fixture. A
// consume checks the on-disk journal itself before returning a fake receipt.
constexpr auto helper = R"python(#!/usr/bin/env python3
import io, json, os, pathlib, shlex, sys, time, types
root = pathlib.Path(__file__).resolve().parent.parent
raw = sys.argv[1:]
input_text = sys.stdin.read()
if pathlib.Path(__file__).name == 'ssh':
    args, source = shlex.split(raw[-1])[2:], input_text
else:
    args, source = raw[1:], pathlib.Path(raw[0]).read_text()
def flag(name, default=''):
    return args[args.index(name)+1] if name in args else default
phase = 'prepare' if '--prepare' in args else 'reconcile' if '--reconcile-only' in args else 'consume'
operation, credit = flag('--operation'), flag('--pending-credit')
states = list((root/'runtime'/'limit-resets').glob('*.json'))
pending = [json.loads(p.read_text()).get('pending', {}) for p in states if p.is_file()]
durable = any(p.get('operation') == operation and p.get('credit') == credit for p in pending)
with (root/'calls.jsonl').open('a') as log:
    log.write(json.dumps(dict(phase=phase, args=args, input=input_text, durable=durable))+'\n')
if phase == 'prepare' and (root/'block-state').exists():
    folder = root/'runtime'/'limit-resets'
    folder.rename(root/'saved-ledger')
    folder.write_text('not a directory')
module = types.ModuleType('tested_reset_helper')
exec(compile(source, 'embedded_limit_resets.py', 'exec'), module.__dict__)
class Account:
    def __init__(self, *unused):
        self.email = 'plan@example.test'
        if (root/'wrong-account').exists() or (phase != 'prepare' and (root/'change-account').exists()):
            self.email = 'different@example.test'
    def read(self, credit_id=None):
        windows = {'five_hour': (1.0, time.time()+7200)}
        if credit_id and (root/'settled').exists():
            return windows, None
        return windows, dict(id='credit-1', title='Fixture reset', program=module.CEDAR,
            available=True, usable=True, remaining=3, expires=time.time()+7200,
            clears=list(module.MAX_REMAINING), requires_limit=False, status='available')
    def spend(self, exact, identifier):
        assert durable and exact['id'] == credit and identifier == operation
        with (root/'provider-calls').open('a') as log:
            log.write(identifier+'\n')
        if (root/'lose-reply').exists():
            os._exit(0)
        return 'reset'
class Claude(Account):
    cli = 'claude'
class Codex(Account):
    cli = 'codex'
module.Claude, module.Codex = Claude, Codex
sys.stdin = io.StringIO(input_text if pathlib.Path(__file__).name != 'ssh' else '')
sys.exit(module.main(args))
)python";

struct Fixture {
    QTemporaryDir directory;
    QDir root{directory.path()};
    QVector<LimitResets::AgentTarget> targets;
    QStringList spent, declined, uncertain;
    int credential_reads{};
    std::unique_ptr<LimitResets> controller;

    Fixture() {
        require(directory.isValid() && root.mkpath(QStringLiteral("bin")), "fixture root");
        for (const auto* program : {"python3", "ssh"}) {
            const auto path = root.filePath(QStringLiteral("bin/") + QLatin1String(program));
            write(path, helper);
            require(
                QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner),
                "executable helper");
        }
        targets.append({{},
                        QStringLiteral("claude"),
                        QStringLiteral("visiting"),
                        QStringLiteral("account-home"),
                        true,
                        QStringLiteral("plan@example.test"),
                        {}});
    }
    void start() {
        controller = std::make_unique<LimitResets>(
            [this] { return targets; },
            [this](const QString& id) { return targets.at(id.toInt()); },
            [this](const QString& name) { return root.filePath(QStringLiteral("bin/") + name); },
            [this] {
                ++credential_reads;
                return QByteArray(R"({"claudeAiOauth":{"accessToken":"fixture"}})");
            },
            root.filePath(QStringLiteral("runtime")));
        controller->setSettings({.automatic = false});
        QObject::connect(controller.get(), &LimitResets::spent,
                         [this](const QString&, const QString& cli, const QString&, const QString&,
                                const QString&) { spent << cli; });
        QObject::connect(
            controller.get(), &LimitResets::declined,
            [this](const QString&, const QString&, const QString& reason) { declined << reason; });
        QObject::connect(
            controller.get(), &LimitResets::uncertain,
            [this](const QString&, const QString&, const QString& reason) { uncertain << reason; });
    }
    [[nodiscard]] QList<QJsonObject> calls() const {
        QList<QJsonObject> result;
        for (const auto& line : read(root.filePath(QStringLiteral("calls.jsonl"))).split('\n'))
            if (!line.isEmpty())
                result.append(QJsonDocument::fromJson(line).object());
        return result;
    }
    [[nodiscard]] QString journal() const {
        const QDir folder(root.filePath(QStringLiteral("runtime/limit-resets")));
        const auto files = folder.entryList({QStringLiteral("*.json")}, QDir::Files);
        require(files.size() == 1, "one target journal");
        return folder.filePath(files.first());
    }
};

void settingsReadFromTheConfig() {
    const auto defaults = lapis::desktop::parse_limit_resets(QJsonValue());
    require(defaults == lapis::desktop::LimitResetSettings{}, "lapis defaults without a setting");
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

void admissionAndAccountIsolation() {
    Fixture f;
    f.start();
    f.controller->useNow(QStringLiteral("0"));
    f.controller->useNow(QStringLiteral("0"));
    require(waitFor([&] { return f.spent.size() == 1; }), "prepared reset completes");
    const auto calls = f.calls();
    require(calls.size() == 2 && calls[0].value("phase") == QLatin1String("prepare") &&
                calls[1].value("phase") == QLatin1String("consume") &&
                calls[1].value("durable").toBool(),
            "one prepare and one consume only after durable admission");
    require(f.declined.size() == 1, "concurrent manual reset was refused");
    require(f.credential_reads == 0, "a visiting plan must not read the machine keychain");
    const auto args = calls[1].value("args").toArray();
    require(args.contains(QStringLiteral("visiting")) &&
                args.contains(QStringLiteral("account-home")) &&
                args.contains(QStringLiteral("--expected-email")),
            "selected plan and identity carried to consume");
    require(
        !QJsonDocument::fromJson(read(f.journal())).object().contains(QStringLiteral("pending")),
        "successful receipt retires the pending operation");
    QLockFile another_controller(f.journal() + QStringLiteral(".lock"));
    require(another_controller.tryLock(0), "completed operation releases its process lock");
    f.controller->useNow(QStringLiteral("0"));
    require(f.declined.size() == 2 && f.calls().size() == 2,
            "another controller's account lock prevents overlapping reset helpers");
}

void interruptedConsumeReconcilesWithoutReplay() {
    Fixture f;
    write(f.root.filePath(QStringLiteral("lose-reply")), "yes");
    f.start();
    f.controller->useNow(QStringLiteral("0"));
    require(waitFor([&] { return f.uncertain.size() == 1; }),
            "missing receipt reports uncertainty");
    const auto original =
        QJsonDocument::fromJson(read(f.journal())).object().value("pending").toObject();
    require(!original.value("operation").toString().isEmpty(), "pending operation persisted");
    f.controller.reset();
    f.start();
    f.controller->useNow(QStringLiteral("0"));
    require(waitFor([&] { return f.uncertain.size() == 2; }),
            "restart reconciles available credit conservatively");
    auto calls = f.calls();
    require(calls.size() == 4 && calls.last().value("phase") == QLatin1String("reconcile"),
            "restart does not repeat consume");
    require(QJsonDocument::fromJson(read(f.journal())).object().value("pending").toObject() ==
                original,
            "uncertain identity never expires or changes");
    write(f.root.filePath(QStringLiteral("settled")), "yes");
    f.controller->useNow(QStringLiteral("0"));
    require(waitFor([&] { return !f.declined.isEmpty(); }),
            "absent credit settles without claiming a new reset");
    calls = f.calls();
    int consumes = 0;
    for (const auto& call : calls)
        consumes += call.value("phase") == QLatin1String("consume");
    require(consumes == 1 && f.spent.isEmpty(),
            "uncertain and expired credits are never replayed or claimed spent");
}

void journalAndIdentityFailuresRefuseBeforeConsume() {
    for (const auto* marker : {"block-state", "wrong-account", "change-account"}) {
        Fixture f;
        write(f.root.filePath(QLatin1String(marker)), "yes");
        f.start();
        f.controller->useNow(QStringLiteral("0"));
        require(waitFor([&] { return !f.declined.isEmpty(); }), "failure is visible");
        const auto calls = f.calls();
        require(calls[0].value("phase") == QLatin1String("prepare") &&
                    read(f.root.filePath(QStringLiteral("provider-calls"))).isEmpty(),
                "storage or selected-account mismatch cannot call the provider consume");
    }
    Fixture f;
    write(f.root.filePath(QStringLiteral("lose-reply")), "yes");
    f.start();
    f.controller->useNow(QStringLiteral("0"));
    require(waitFor([&] { return !f.uncertain.isEmpty(); }), "create pending fixture");
    const auto path = f.journal();
    f.controller.reset();
    write(path, R"({"v":1,"attempts":{},"pending":{"credit":"broken"}})");
    const auto count = f.calls().size();
    f.start();
    f.controller->useNow(QStringLiteral("0"));
    require(waitFor([&] { return f.uncertain.size() == 2; }),
            "malformed pending state fails closed");
    require(f.calls().size() == count, "invalid journal invokes no helper");
}

void configuredTargetsUseTheirOwnRoutes() {
    Fixture f;
    f.targets[0].account.clear();
    f.targets[0].hasHome = false;
    f.targets.append({QStringLiteral("fixture-host"),
                      QStringLiteral("codex"),
                      QStringLiteral("second-plan"),
                      {},
                      false,
                      QStringLiteral("plan@example.test"),
                      {}});
    f.targets.append({{}, QStringLiteral("shell"), {}, {}, false, {}, {}});
    f.start();
    require(!f.controller->canUseNow(QStringLiteral("2")), "shells have no reset action");
    f.controller->setSettings(
        {.automatic = true, .minBlockedMinutes = 30, .keepCredits = 1, .salvageHours = 6});
    f.controller->sweep();
    require(waitFor([&] { return f.spent.size() == 2; }), "both selected account routes complete");
    const auto calls = f.calls();
    require(calls.size() == 4 && f.credential_reads == 1,
            "one check per account and keychain only for the local own sign-in");
    for (const auto& call : calls) {
        const auto args = call.value("args").toArray();
        require(args.contains(QStringLiteral("--keep")) && args.contains(QStringLiteral("1")) &&
                    args.contains(QStringLiteral("30")),
                "policy settings reach each target");
        if (args.contains(QStringLiteral("--codex-plan"))) {
            require(
                args.contains(QStringLiteral("second-plan")) &&
                    args.contains(QStringLiteral("fixture-host")) &&
                    !args.contains(QStringLiteral("--claude-credentials-stdin")) &&
                    call.value("input").toString().contains(QStringLiteral("def plan(")),
                "remote target gets its selected plan and helper script, never local credentials");
        } else {
            require(call.value("input").toString() ==
                        QLatin1String("{\"claudeAiOauth\":{\"accessToken\":\"fixture\"}}\n"),
                    "local credentials travel only over stdin");
        }
    }
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    try {
        settingsReadFromTheConfig();
        admissionAndAccountIsolation();
        interruptedConsumeReconcilesWithoutReplay();
        journalAndIdentityFailuresRefuseBeforeConsume();
        configuredTargetsUseTheirOwnRoutes();
    } catch (const std::exception& error) {
        std::cerr << "limit_resets_test: " << error.what() << '\n';
        return 1;
    }
    std::cout << "limit_resets_test: PASS\n";
}
