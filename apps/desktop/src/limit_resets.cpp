#include "limit_resets.hpp"

#include "limit_resets_script.hpp"
#include "platform/reset_journal.hpp"
#include "platform/updater_process.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QLockFile>
#include <QSaveFile>
#include <QSet>
#include <QThreadPool>
#include <QUuid>

#include <algorithm>
#include <utility>

namespace lapis::desktop {
namespace {
constexpr int kSweepMs = 5 * 60 * 1000;
constexpr int kHelperTimeoutMs = 3 * 60 * 1000;
constexpr qint64 kRetryMs = qint64{60} * 60 * 1000;
constexpr qint64 kSettledMs = qint64{30} * 24 * 60 * 60 * 1000;
constexpr qsizetype kOutputLimit = qsizetype{1024} * 1024;
constexpr qsizetype kErrorLimit = qsizetype{64} * 1024;
constexpr int kJournalLimit = 256;
constexpr int kConcurrentLimit = 8;
constexpr int kAttemptLimit = 128;

bool supported(const QString& cli) {
    return cli == QLatin1String("claude") || cli == QLatin1String("codex");
}
QString quoted(const QString& word) {
    return QLatin1Char('\'') + QString(word).replace(QLatin1Char('\''), QStringLiteral("'\\''")) +
           QLatin1Char('\'');
}
QString targetKey(const LimitResets::AgentTarget& target) {
    const QJsonArray identity{target.machine, target.cli, target.account, target.home,
                              target.hasHome};
    return QString::fromLatin1(
        QCryptographicHash::hash(QJsonDocument(identity).toJson(QJsonDocument::Compact),
                                 QCryptographicHash::Sha256)
            .toHex());
}
QString why(const QString& decision) {
    if (decision == QLatin1String("restore"))
        return QStringLiteral("blocked");
    return decision == QLatin1String("salvage") ? QStringLiteral("expiring")
                                                : QStringLiteral("asked");
}
bool validState(const QJsonObject& state) {
    if (state.value(QStringLiteral("v")).toInt() != 1)
        return false;
    const auto attempts = state.value(QStringLiteral("attempts"));
    if (!attempts.isObject() || attempts.toObject().size() > kAttemptLimit)
        return false;
    for (const auto& value : attempts.toObject())
        if (!value.isDouble() || value.toDouble() < 0)
            return false;
    if (!state.contains(QStringLiteral("pending")))
        return true;
    const auto pending = state.value(QStringLiteral("pending")).toObject();
    for (const auto* field : {"operation", "credit", "attempt", "email", "decision"}) {
        const auto value = pending.value(QLatin1String(field));
        if (!value.isString() || value.toString().isEmpty() || value.toString().size() > 512)
            return false;
    }
    return !QUuid(pending.value(QStringLiteral("operation")).toString()).isNull();
}
QJsonObject unexpiredAttempts(const QJsonObject& state) {
    const auto now = QDateTime::currentMSecsSinceEpoch();
    QJsonObject kept;
    const auto attempts = state.value(QStringLiteral("attempts")).toObject();
    for (auto it = attempts.begin(); it != attempts.end(); ++it)
        if (it.value().toDouble() > static_cast<double>(now) && it.key().size() <= 512)
            kept.insert(it.key(), it.value());
    return kept;
}
} // namespace

struct LimitResets::Run {
    AgentTarget target;
    QString key;
    QString state_path;
    bool asked{};
    Phase phase{Phase::prepare};
    QJsonObject state;
    std::unique_ptr<QLockFile> lock;
    QPointer<UpdaterProcess> process;
    QByteArray credentials;
    QByteArray output;
    qsizetype error_bytes{};
    bool oversized{};
    bool timed_out{};

    void drain() {
        if (!process)
            return;
        const auto bytes = process->readAllStandardOutput();
        error_bytes += process->readAllStandardError().size();
        if (output.size() + bytes.size() > kOutputLimit || error_bytes > kErrorLimit)
            oversized = true;
        if (oversized)
            process->stopGroup();
        else
            output.append(bytes);
    }
};

LimitResetSettings parse_limit_resets(const QJsonValue& value) {
    LimitResetSettings settings;
    const auto object = value.toObject();
    if (object.value(QStringLiteral("auto")).isBool())
        settings.automatic = object.value(QStringLiteral("auto")).toBool();
    if (const auto v = object.value(QStringLiteral("minBlockedMinutes")); v.isDouble())
        settings.minBlockedMinutes = std::max(0.0, v.toDouble());
    if (const auto v = object.value(QStringLiteral("keepCredits")); v.isDouble())
        settings.keepCredits = std::max(0, v.toInt());
    if (const auto v = object.value(QStringLiteral("salvageHours")); v.isDouble())
        settings.salvageHours = std::max(0.0, v.toDouble());
    return settings;
}

LimitResets::LimitResets(Agents agents, Agent agent, Program program, Credentials credentials,
                         const QString& folder, QObject* parent)
    : QObject(parent), agents_(std::move(agents)), agent_(std::move(agent)),
      program_(std::move(program)), credentials_(std::move(credentials)),
      script_path_(QDir(folder).filePath(QStringLiteral("limit_resets.py"))),
      state_folder_(QDir(folder).filePath(QStringLiteral("limit-resets"))) {
    QDir().mkpath(state_folder_, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    QSaveFile script(script_path_);
    helper_ready_ =
        script.open(QIODevice::WriteOnly) &&
        script.setPermissions(QFile::ReadOwner | QFile::WriteOwner) &&
        script.write(kLimitResetsScript) == static_cast<qint64>(sizeof(kLimitResetsScript) - 1) &&
        script.commit();
    timer_.setInterval(kSweepMs);
    connect(&timer_, &QTimer::timeout, this, &LimitResets::sweep);
    setSettings(settings_);
}
LimitResets::~LimitResets() {
    for (const auto& run : std::as_const(running_))
        if (run->process)
            run->process->stopGroup();
}
void LimitResets::setSettings(LimitResetSettings settings) {
    settings_ = settings;
    if (!settings_.automatic)
        timer_.stop();
    else if (!timer_.isActive()) {
        timer_.start();
        QTimer::singleShot(60 * 1000, this, [this] {
            if (settings_.automatic)
                sweep();
        });
    }
}
void LimitResets::sweep() {
    QSet<QString> seen;
    for (const auto& target : agents_()) {
        const auto key = targetKey(target);
        if (!supported(target.cli) || seen.contains(key))
            continue;
        seen.insert(key);
        run(target, false);
    }
}
bool LimitResets::canUseNow(const QString& id) const { return supported(agent_(id).cli); }
void LimitResets::useNow(const QString& id) {
    auto target = agent_(id);
    if (supported(target.cli))
        run(std::move(target), true);
}
bool LimitResets::current(const std::shared_ptr<Run>& run) const {
    return running_.value(run->key) == run;
}
void LimitResets::refuse(const std::shared_ptr<Run>& run, const QString& reason) {
    if (!current(run))
        return;
    if (run->lock)
        run->lock->unlock();
    running_.remove(run->key);
    if (!run->state.value(QStringLiteral("pending")).toObject().isEmpty()) {
        if (run->asked || !uncertain_notified_.contains(run->key)) {
            uncertain_notified_.insert(run->key);
            emit uncertain(run->target.machine, run->target.cli, reason);
        }
    } else if (run->asked)
        emit declined(run->target.machine, run->target.cli, reason);
    else
        qWarning().noquote() << "Limit reset check:" << reason;
}
void LimitResets::run(AgentTarget target, bool asked) {
    const auto key = targetKey(target);
    if (running_.contains(key) || running_.size() >= kConcurrentLimit) {
        if (asked)
            emit declined(target.machine, target.cli,
                          QStringLiteral("a reset check is already running"));
        return;
    }
    auto pending = std::make_shared<Run>();
    pending->target = std::move(target);
    pending->key = key;
    pending->asked = asked;
    pending->state_path = QDir(state_folder_).filePath(key + QStringLiteral(".json"));
    running_.insert(key, pending);
    pending->lock = std::make_unique<QLockFile>(pending->state_path + QStringLiteral(".lock"));
    pending->lock->setStaleLockTime(0); // Age alone cannot expire a live reset operation.
    if (!pending->lock->tryLock(0)) {
        refuse(pending, QStringLiteral("the account reset journal is locked or unavailable"));
        return;
    }
    if (!helper_ready_ || !pending->target.refusal.isEmpty()) {
        refuse(pending, pending->target.refusal.isEmpty()
                            ? QStringLiteral("reset helper unavailable")
                            : pending->target.refusal);
        return;
    }
    if (loadState(pending))
        readCredentials(pending);
}
bool LimitResets::loadState(const std::shared_ptr<Run>& pending) {
    QFile state(pending->state_path);
    if (state.exists()) {
        QJsonParseError error{};
        if (QFileInfo(state).isSymLink() || !state.open(QIODevice::ReadOnly) ||
            state.size() > 65536) {
            refuse(pending, QStringLiteral("reset journal unreadable; no reset was requested"));
            return false;
        }
        const auto doc = QJsonDocument::fromJson(state.readAll(), &error);
        pending->state = doc.object();
        if (error.error != QJsonParseError::NoError || !validState(pending->state)) {
            refuse(pending, QStringLiteral("reset journal invalid; no reset was requested"));
            return false;
        }
    } else if (QDir(state_folder_).entryList({QStringLiteral("*.json")}, QDir::Files).size() >=
               kJournalLimit) {
        refuse(pending, QStringLiteral("reset journal capacity reached"));
        return false;
    }
    return true;
}
void LimitResets::readCredentials(const std::shared_ptr<Run>& pending) {
    const auto& t = pending->target;
    const bool ownClaude = t.machine.isEmpty() && t.cli == QLatin1String("claude") &&
                           (t.account.isEmpty() || (t.hasHome && t.home.isEmpty()));
    if (!ownClaude || !credentials_) {
        start(pending);
        return;
    }
    // Only immutable values cross threads; the guard is read on the GUI thread.
    QThreadPool::globalInstance()->start([guard = QPointer<LimitResets>(this), pending,
                                          read = credentials_] {
        const auto credentials = read();
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [guard, pending, credentials] {
                if (guard && guard->current(pending)) {
                    if (credentials.isEmpty()) {
                        guard->refuse(pending,
                                      QStringLiteral(
                                          "the local Claude Code keychain sign-in is unavailable"));
                        return;
                    }
                    pending->credentials = credentials;
                    guard->start(pending);
                }
            },
            Qt::QueuedConnection);
    });
}
QStringList LimitResets::arguments(const Run& run) const {
    QStringList args;
    if (run.asked)
        args << QStringLiteral("--now") << run.target.cli;
    else if (settings_.automatic)
        args << QStringLiteral("--apply");
    args << QStringLiteral("--min-blocked-minutes") << QString::number(settings_.minBlockedMinutes)
         << QStringLiteral("--keep") << QString::number(settings_.keepCredits)
         << QStringLiteral("--salvage-hours") << QString::number(settings_.salvageHours);
    args << (run.target.cli == QLatin1String("claude") ? QStringLiteral("--claude-plan")
                                                       : QStringLiteral("--codex-plan"))
         << run.target.account;
    args << (run.target.cli == QLatin1String("claude") ? QStringLiteral("--claude-plan-home")
                                                       : QStringLiteral("--codex-plan-home"))
         << (run.target.hasHome
                 ? (run.target.home.isEmpty() ? QStringLiteral("local") : run.target.home)
                 : QString());
    args << QStringLiteral("--machine")
         << (run.target.machine.isEmpty() ? QStringLiteral("local") : run.target.machine);
    if (run.phase == Phase::prepare)
        args << QStringLiteral("--prepare");
    else {
        const auto operation = run.state.value(QStringLiteral("pending")).toObject();
        args << QStringLiteral("--operation")
             << operation.value(QStringLiteral("operation")).toString()
             << QStringLiteral("--pending-credit")
             << operation.value(QStringLiteral("credit")).toString()
             << QStringLiteral("--expected-email")
             << operation.value(QStringLiteral("email")).toString();
        if (run.phase == Phase::reconcile)
            args << QStringLiteral("--reconcile-only");
    }
    const auto attempts = unexpiredAttempts(run.state);
    if (!attempts.isEmpty())
        args << QStringLiteral("--attempted") << attempts.keys();
    if (!run.credentials.isEmpty())
        args << QStringLiteral("--claude-credentials-stdin");
    return args;
}
void LimitResets::start(const std::shared_ptr<Run>& run) {
    if (!current(run))
        return;
    auto* process = new UpdaterProcess(this);
    run->process = process;
    run->output.clear();
    run->error_bytes = 0;
    run->oversized = false;
    run->timed_out = false;
    QByteArray input;
    if (run->target.machine.isEmpty()) {
        process->setProgram(program_(QStringLiteral("python3")));
        process->setArguments(QStringList{script_path_} + arguments(*run));
        if (!run->credentials.isEmpty())
            input = run->credentials.simplified() + '\n';
    } else {
        QStringList words{QStringLiteral("python3"), QStringLiteral("-")};
        for (const auto& word : arguments(*run))
            words << quoted(word);
        process->setProgram(program_(QStringLiteral("ssh")));
        process->setArguments({QStringLiteral("-o"), QStringLiteral("BatchMode=yes"),
                               QStringLiteral("-o"), QStringLiteral("ConnectTimeout=10"),
                               QStringLiteral("-o"), QStringLiteral("ControlPath=none"),
                               QStringLiteral("-T"), QStringLiteral("--"), run->target.machine,
                               words.join(QLatin1Char(' '))});
        input = kLimitResetsScript;
    }
    const auto drain = [run, process] {
        if (run->process == process)
            run->drain();
    };
    connect(process, &QProcess::readyReadStandardOutput, process, drain);
    connect(process, &QProcess::readyReadStandardError, process, drain);
    connect(process, &QProcess::finished, this, [this, run, process] {
        if (run->process == process)
            finish(run);
    });
    connect(process, &QProcess::errorOccurred, this,
            [this, run, process](QProcess::ProcessError error) {
                if (run->process == process && error == QProcess::FailedToStart)
                    finish(run);
            });
    QTimer::singleShot(kHelperTimeoutMs, process, [run, process] {
        if (run->process != process)
            return;
        run->timed_out = true;
        if (run->process)
            run->process->stopGroup();
    });
    process->start();
    process->write(input);
    process->closeWriteChannel();
}
void LimitResets::finish(const std::shared_ptr<Run>& run) {
    if (!current(run) || !run->process)
        return;
    auto* process = run->process.data();
    run->drain();
    process->deleteLater();
    QJsonParseError error{};
    const auto document = QJsonDocument::fromJson(run->output, &error);
    const auto accounts = document.object().value(QStringLiteral("accounts")).toArray();
    if (run->oversized || run->timed_out || process->error() == QProcess::FailedToStart ||
        process->exitCode() != 0 || error.error != QJsonParseError::NoError ||
        accounts.size() != 1) {
        refuse(run, QStringLiteral("reset helper failed; any pending outcome remains unknown"));
        return;
    }
    const auto account = accounts.first().toObject();
    if (account.value(QStringLiteral("cli")).toString() != run->target.cli) {
        refuse(run, QStringLiteral("reset helper returned a different CLI"));
        return;
    }
    if (run->phase == Phase::prepare)
        prepared(run, account);
    else
        completed(run, account);
}
void LimitResets::prepared(const std::shared_ptr<Run>& run, const QJsonObject& account) {
    const auto email = account.value(QStringLiteral("email")).toString().toLower();
    auto pending = run->state.value(QStringLiteral("pending")).toObject();
    const auto expected = pending.isEmpty() ? run->target.email.toLower()
                                            : pending.value(QStringLiteral("email")).toString();
    if (!account.value(QStringLiteral("error")).toString().isEmpty() || email.isEmpty() ||
        (!expected.isEmpty() && email != expected)) {
        refuse(run, QStringLiteral("selected plan identity could not be verified"));
        return;
    }
    if (!pending.isEmpty()) {
        run->phase = Phase::reconcile;
        start(run);
        return;
    }
    const auto decision = account.value(QStringLiteral("action")).toString();
    const auto key = account.value(QStringLiteral("attempt")).toString();
    const auto credit = account.value(QStringLiteral("credit_id")).toString();
    if (account.value(QStringLiteral("decision")) != QLatin1String("prepared") ||
        (!run->asked && !settings_.automatic) || key.isEmpty() || credit.isEmpty() ||
        (decision != QLatin1String("restore") && decision != QLatin1String("salvage") &&
         decision != QLatin1String("now"))) {
        refuse(run, decision.isEmpty() ? QStringLiteral("no eligible reset") : decision);
        return;
    }
    const auto attempts = unexpiredAttempts(run->state);
    if (attempts.size() >= kAttemptLimit || key.size() > 512 || credit.size() > 256) {
        refuse(run, QStringLiteral("reset attempt capacity reached"));
        return;
    }
    pending = {{QStringLiteral("operation"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
               {QStringLiteral("credit"), credit},
               {QStringLiteral("attempt"), key},
               {QStringLiteral("email"), email},
               {QStringLiteral("decision"), decision}};
    const QJsonObject state{{QStringLiteral("v"), 1},
                            {QStringLiteral("pending"), pending},
                            {QStringLiteral("attempts"), attempts}};
    persist(run, state, [this, run] {
        if (!run->asked && !settings_.automatic) {
            auto cancelled = run->state;
            cancelled.remove(QStringLiteral("pending"));
            persist(run, cancelled, [this, run] {
                refuse(run, QStringLiteral("automatic resets were disabled before submission"));
            });
            return;
        }
        run->phase = Phase::consume;
        start(run);
    });
}
void LimitResets::persist(const std::shared_ptr<Run>& run, const QJsonObject& state,
                          const std::function<void()>& done) {
    if (!validState(state)) {
        refuse(run, QStringLiteral("invalid reset journal update; no further reset was requested"));
        return;
    }
    // Each target has its own file and only one active run. This serializes
    // writes for that account without blocking the GUI or unrelated accounts.
    QThreadPool::globalInstance()->start([guard = QPointer<LimitResets>(this), run, state, done] {
        const bool saved = platform::write_reset_journal(run->state_path, state);
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [guard, run, state, done, saved] {
                if (!guard || !guard->current(run))
                    return;
                if (!saved) {
                    guard->refuse(
                        run,
                        QStringLiteral(
                            "reset journal could not be saved; no further reset was requested"));
                    return;
                }
                run->state = state;
                done();
            },
            Qt::QueuedConnection);
    });
}
void LimitResets::completed(const std::shared_ptr<Run>& run, const QJsonObject& account) {
    const auto result = account.value(QStringLiteral("result")).toString();
    const bool consumed = result == QLatin1String("reset");
    const bool settled = result == QLatin1String("settled");
    const bool refused =
        run->phase == Phase::consume &&
        (result == QLatin1String("refused") || result == QLatin1String("nothing_to_reset") ||
         result == QLatin1String("rate_limited") || result == QLatin1String("http_429") ||
         result == QLatin1String("http_401") || result == QLatin1String("http_403"));
    const auto operation = run->state.value(QStringLiteral("pending")).toObject();
    if ((consumed || settled) && (account.value(QStringLiteral("operation")).toString() !=
                                      operation.value(QStringLiteral("operation")).toString() ||
                                  account.value(QStringLiteral("credit_id")).toString() !=
                                      operation.value(QStringLiteral("credit")).toString() ||
                                  account.value(QStringLiteral("email")).toString().toLower() !=
                                      operation.value(QStringLiteral("email")).toString())) {
        refuse(run, QStringLiteral("reset receipt identity does not match the admitted operation"));
        return;
    }
    if (!consumed && !settled && !refused) {
        refuse(run, QStringLiteral("reset outcome is unknown; it will not be submitted again"));
        return;
    }
    auto attempts = unexpiredAttempts(run->state);
    const auto pending = run->state.value(QStringLiteral("pending")).toObject();
    attempts.insert(pending.value(QStringLiteral("attempt")).toString(),
                    static_cast<double>(QDateTime::currentMSecsSinceEpoch() +
                                        (refused ? kRetryMs : kSettledMs)));
    const QJsonObject state{{QStringLiteral("v"), 1}, {QStringLiteral("attempts"), attempts}};
    persist(run, state, [this, run, account, consumed, settled, pending] {
        run->lock->unlock();
        running_.remove(run->key);
        uncertain_notified_.remove(run->key);
        if (consumed)
            emit spent(run->target.machine, run->target.cli,
                       pending.value(QStringLiteral("email")).toString(),
                       account.value(QStringLiteral("credit"))
                           .toObject()
                           .value(QStringLiteral("title"))
                           .toString(),
                       why(pending.value(QStringLiteral("decision")).toString()));
        else if (run->asked)
            emit declined(
                run->target.machine, run->target.cli,
                settled
                    ? QStringLiteral(
                          "the previous credit is no longer available; no new reset was requested")
                    : QStringLiteral("the provider declined the reset"));
    });
}
} // namespace lapis::desktop
