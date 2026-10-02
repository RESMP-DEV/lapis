#include "limit_resets.hpp"
#include "platform/reset_journal.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QTemporaryDir>
#include <QThreadPool>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
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
    def __init__(self, *unused, **options):
        self.org, self.account = 'fixture-org', 'fixture-account'
        if phase != 'prepare' and (root/'change-account-id').exists():
            self.org, self.account = 'another-org', 'another-account'
        self.email = 'plan@example.test'
        if (root/'wrong-account').exists() or (phase != 'prepare' and (root/'change-account').exists()):
            self.email = 'different@example.test'
    def read(self, credit_id=None):
        if (root/'listing-fails').exists():
            raise module.Unavailable('fixture credit listing unavailable')
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
        if (root/'provider-refusal').exists():
            return (root/'provider-refusal').read_text().strip()
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
    std::atomic<int> credential_reads{};
    std::function<void()> before_credentials;
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
        for (auto& target : targets)
            if (!target.account.isEmpty())
                target.credential = root.filePath(target.account + QStringLiteral(".credential"));
        controller = std::make_unique<LimitResets>(
            [this] { return targets; },
            [this](const QString& id) { return targets.at(id.toInt()); },
            [this](const QString& name) { return root.filePath(QStringLiteral("bin/") + name); },
            [this] {
                if (before_credentials)
                    before_credentials();
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
    require(args.contains(QStringLiteral("--claude-token-file")) &&
                args.contains(f.targets[0].credential),
            "the workspace's exact credential override reaches the helper");
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
    require(waitFor([&] { return f.declined.size() == 2; }) && f.calls().size() == 3 &&
                f.calls().last().value("phase") == QLatin1String("prepare"),
            "another controller's account lock allows discovery but prevents consume");
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
    for (const auto* marker : {"change-account", "change-account-id"}) {
        const auto before = f.uncertain.size();
        write(f.root.filePath(QLatin1String(marker)), "yes");
        f.controller->useNow(QStringLiteral("0"));
        require(waitFor([&] { return f.uncertain.size() == before + 1; }),
                "identity change during reconciliation stays uncertain");
        require(QJsonDocument::fromJson(read(f.journal())).object().value("pending").toObject() ==
                    original,
                "a reconciliation refusal cannot retire the original operation");
        QFile::remove(f.root.filePath(QLatin1String(marker)));
    }
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
    for (const auto* marker :
         {"block-state", "wrong-account", "change-account", "change-account-id"}) {
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
    const auto valid_pending = QJsonDocument::fromJson(read(path)).object();
    f.controller.reset();
    write(path,
          R"({"v":2,"format":"lapis-reset-journal","attempts":{},"pending":{"credit":"broken"}})");
    const auto count = f.calls().size();
    f.start();
    f.controller->useNow(QStringLiteral("0"));
    require(waitFor([&] { return f.uncertain.size() == 2; }),
            "malformed pending state fails closed");
    require(f.calls().size() == count + 1 &&
                f.calls().last().value("phase") == QLatin1String("prepare"),
            "invalid journal allows account discovery but never consume");
    for (const auto& credit :
         {QString(257, QLatin1Char('x')), QStringLiteral("credit with space")}) {
        f.controller.reset();
        auto invalid = valid_pending;
        auto pending = invalid.value(QStringLiteral("pending")).toObject();
        pending.insert(QStringLiteral("credit"), credit);
        invalid.insert(QStringLiteral("pending"), pending);
        const auto bytes = QJsonDocument(invalid).toJson();
        write(path, bytes);
        const auto before = f.calls().size();
        f.uncertain.clear();
        f.start();
        f.controller->useNow(QStringLiteral("0"));
        require(waitFor([&] { return !f.uncertain.isEmpty(); }),
                "invalid helper credit identity is visible");
        require(f.calls().size() == before + 1 && read(path) == bytes,
                "native validation preserves the invalid journal and never submits its credit");
    }
}
void structuredHelperReasonsRemainVisible() {
    Fixture f;
    write(f.root.filePath(QStringLiteral("listing-fails")), "yes");
    f.start();
    f.controller->useNow(QStringLiteral("0"));
    require(waitFor([&] { return !f.declined.isEmpty(); }), "helper failure surfaced");
    require(f.declined.last().contains(QStringLiteral("fixture credit listing unavailable")),
            "structured helper reason is retained without relying on stderr");
    Fixture refused;
    write(refused.root.filePath(QStringLiteral("provider-refusal")), "not_limited");
    refused.start();
    refused.controller->useNow(QStringLiteral("0"));
    require(waitFor([&] { return !refused.declined.isEmpty(); }), "business refusal is reported");
    require(refused.declined.last().contains(QStringLiteral("not_limited")) &&
                !QJsonDocument::fromJson(read(refused.journal())).object().contains("pending"),
            "definitive refusal retires pending state and retains its reason");
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
    require(waitFor([&] { return f.spent.size() == 1; }), "the remote plan's route completes");
    // Drain the shared pool deterministically: a sentinel queued after the sweep
    // has finished every task queued before it, so a wrongly queued credential
    // read is observed instead of inferred from elapsed time.
    std::atomic<bool> sentinel_ran{false};
    QThreadPool::globalInstance()->start([&sentinel_ran] { sentinel_ran = true; });
    require(waitFor([&] { return sentinel_ran.load(); }),
            "the credential queue is drained before the assertion");
    require(f.credential_reads == 0,
            "a sweep skips this Mac's own Claude sign-in rather than raise the keychain prompt");
    f.controller->useNow(QStringLiteral("0"));
    require(waitFor([&] { return f.spent.size() == 2; }), "asking spends the local own reset");
    const auto calls = f.calls();
    require(calls.size() == 4 && f.credential_reads == 1,
            "one check per account, and the keychain read only when asked");
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

void journalCapacityReclaimsOnlyExpiredClosedRecords() {
    using namespace lapis::desktop::platform;
    for (const bool all_pending : {false, true}) {
        Fixture f;
        f.start();
        const QDir folder(f.root.filePath(QStringLiteral("runtime/limit-resets")));
        const QJsonObject closed{
            {QStringLiteral("v"), reset_journal_version},
            {QStringLiteral("format"), QStringLiteral("lapis-reset-journal")},
            {QStringLiteral("attempts"), QJsonObject{{QStringLiteral("old"), 0}}}};
        for (int i = 0; i < reset_journal_max_files; ++i) {
            auto state = closed;
            if (all_pending || i == 0)
                state.insert(QStringLiteral("pending"),
                             QJsonObject{{QStringLiteral("credit"), QStringLiteral("unknown")}});
            write(folder.filePath(QString::number(i) + QStringLiteral(".json")),
                  QJsonDocument(state).toJson(QJsonDocument::Compact));
        }
        f.controller->useNow(QStringLiteral("0"));
        require(waitFor([&] { return !f.spent.isEmpty() || !f.declined.isEmpty(); }),
                "capacity admission completes");
        require(QFile::exists(folder.filePath(QStringLiteral("0.json"))) &&
                    folder.entryList({QStringLiteral("*.json")}, QDir::Files).size() ==
                        reset_journal_max_files,
                "pending records are retained and the file bound holds");
        require(all_pending ? f.spent.isEmpty() && f.calls().size() == 1 : f.spent.size() == 1,
                "expired closed records make room; pending records never authorize eviction");
    }
}

void closingDuringCredentialLookupCannotLaunchAHelper() {
    struct Gate {
        std::mutex mutex;
        std::condition_variable changed;
        bool started{}, released{};
    };
    auto gate = std::make_shared<Gate>();
    Fixture f;
    f.targets[0].account.clear();
    f.before_credentials = [gate] {
        std::unique_lock lock(gate->mutex);
        gate->started = true;
        gate->changed.notify_all();
        gate->changed.wait(lock, [&] { return gate->released; });
    };
    f.start();
    f.controller->useNow(QStringLiteral("0"));
    require(waitFor([&] {
                const std::lock_guard lock(gate->mutex);
                return gate->started;
            }),
            "credential lookup starts off the GUI thread");
    f.controller.reset();
    {
        const std::lock_guard lock(gate->mutex);
        gate->released = true;
    }
    gate->changed.notify_all();
    require(QThreadPool::globalInstance()->waitForDone(5000), "credential task completes");
    QCoreApplication::sendPostedEvents();
    require(f.calls().isEmpty(), "a late credential result cannot start an orphan reset");
}

void aliasesOnDifferentHostsSharePendingRecovery() {
    Fixture f;
    f.targets.append({QStringLiteral("fixture-host"),
                      QStringLiteral("claude"),
                      QStringLiteral("another-alias"),
                      {},
                      false,
                      QStringLiteral("plan@example.test"),
                      {}});
    write(f.root.filePath(QStringLiteral("lose-reply")), "yes");
    f.start();
    f.controller->useNow(QStringLiteral("0"));
    require(waitFor([&] { return f.uncertain.size() == 1; }), "first host loses its receipt");
    const auto before = read(f.journal());
    f.controller->useNow(QStringLiteral("1"));
    require(waitFor([&] { return f.uncertain.size() == 2; }),
            "another host reconciles the same provider account");
    const auto calls = f.calls();
    require(calls.size() == 4 && calls.last().value("phase") == QLatin1String("reconcile") &&
                read(f.journal()) == before,
            "changing machine or plan alias cannot create another consume operation");
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    try {
        settingsReadFromTheConfig();
        admissionAndAccountIsolation();
        interruptedConsumeReconcilesWithoutReplay();
        journalAndIdentityFailuresRefuseBeforeConsume();
        structuredHelperReasonsRemainVisible();
        configuredTargetsUseTheirOwnRoutes();
        journalCapacityReclaimsOnlyExpiredClosedRecords();
        closingDuringCredentialLookupCannotLaunchAHelper();
        aliasesOnDifferentHostsSharePendingRecovery();
    } catch (const std::exception& error) {
        std::cerr << "limit_resets_test: " << error.what() << '\n';
        return 1;
    }
    std::cout << "limit_resets_test: PASS\n";
}
