#include "limit_resets.hpp"

#include "limit_resets_script.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include <thread>

namespace lapis::desktop {
namespace {
constexpr int kSweepMs = 5 * 60 * 1000;
constexpr int kFirstSweepMs = 60 * 1000;
// The helper's requests can take 200 seconds between them (two accounts, each
// read, spent and read again), plus the ssh connection.
constexpr int kHelperTimeoutMs = 240 * 1000;
constexpr qint64 kRetryLaterMs = qint64{60} * 60 * 1000;
constexpr qint64 kSettledMs = qint64{8} * 24 * 60 * 60 * 1000;
const QStringList& agentClis() {
    static const QStringList clis{QStringLiteral("claude"), QStringLiteral("codex")};
    return clis;
}
QString quoted(const QString& word) {
    return QLatin1Char('\'') + QString(word).replace(QLatin1Char('\''), QStringLiteral("'\\''")) +
           QLatin1Char('\'');
}
QString why(const QString& decision) {
    if (decision == QLatin1String("restore"))
        return QStringLiteral("blocked");
    if (decision == QLatin1String("salvage"))
        return QStringLiteral("expiring");
    return QStringLiteral("asked");
}
} // namespace

LimitResetSettings parse_limit_resets(const QJsonValue& value) {
    LimitResetSettings settings;
    const auto object = value.toObject();
    if (object.value(QStringLiteral("auto")).isBool())
        settings.automatic = object.value(QStringLiteral("auto")).toBool();
    if (const auto minutes = object.value(QStringLiteral("minBlockedMinutes")); minutes.isDouble())
        settings.minBlockedMinutes = std::max(0.0, minutes.toDouble());
    if (const auto keep = object.value(QStringLiteral("keepCredits")); keep.isDouble())
        settings.keepCredits = std::max(0, keep.toInt());
    if (const auto hours = object.value(QStringLiteral("salvageHours")); hours.isDouble())
        settings.salvageHours = std::max(0.0, hours.toDouble());
    return settings;
}

LimitResets::LimitResets(Machines machines, Agent agent, Program program, Credentials credentials,
                         const QString& folder, QObject* parent)
    : QObject(parent), machines_(std::move(machines)), agent_(std::move(agent)),
      program_(std::move(program)), credentials_(std::move(credentials)),
      script_path_(QDir(folder).filePath(QStringLiteral("limit_resets.py"))) {
    QSaveFile script(script_path_);
    if (!QDir().mkpath(folder) || !script.open(QIODevice::WriteOnly)) {
        qWarning() << "Limit resets: cannot write the helper on this Mac:" << script_path_
                   << script.errorString();
    } else {
        script.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
        script.write(kLimitResetsScript);
        if (!script.commit())
            qWarning() << "Limit resets: cannot write the helper:" << script.errorString();
    }
    timer_.setInterval(kSweepMs);
    connect(&timer_, &QTimer::timeout, this, &LimitResets::sweep);
    setSettings(settings_);
}

LimitResets::~LimitResets() {
    for (const auto& process : std::as_const(running_))
        if (process)
            process->kill();
}

void LimitResets::setSettings(const LimitResetSettings& settings) {
    settings_ = settings;
    if (!settings_.automatic) {
        timer_.stop();
    } else if (!timer_.isActive()) {
        timer_.start();
        QTimer::singleShot(kFirstSweepMs, this, [this] {
            if (settings_.automatic)
                sweep();
        });
    }
}

void LimitResets::sweep() {
    for (const auto& machine : machines_())
        run({.machine = machine, .asked = {}, .credential = {}});
}

bool LimitResets::canUseNow(const QString& id) const {
    return agentClis().contains(agent_(id).cli);
}

void LimitResets::useNow(const QString& id) {
    auto agent = agent_(id);
    if (agentClis().contains(agent.cli))
        run({.machine = std::move(agent.machine),
             .asked = std::move(agent.cli),
             .credential = std::move(agent.credential)});
}

void LimitResets::run(const Check& check) {
    if (running_.contains(check.machine)) {
        if (!check.asked.isEmpty())
            emit declined(check.machine, check.asked,
                          QStringLiteral("a check is already running there; try again shortly"));
        return;
    }
    running_.insert(check.machine, nullptr);
    if (!check.machine.isEmpty() || !check.credential.isEmpty() || !credentials_) {
        start(check, {});
        return;
    }
    // Reading the keychain can wait on the system's permission prompt.
    const QPointer<LimitResets> self(this);
    std::thread([self, check, read = credentials_] {
        auto credentials = read();
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [self, check, credentials = std::move(credentials)] {
                if (self)
                    self->start(check, credentials);
            },
            Qt::QueuedConnection);
    }).detach();
}

QStringList LimitResets::arguments(const Check& check, bool withCredentials) const {
    QStringList arguments;
    // Asking for one CLI's reset spends that one alone.
    if (settings_.automatic && check.asked.isEmpty())
        arguments << QStringLiteral("--apply");
    if (!check.asked.isEmpty())
        arguments << QStringLiteral("--now") << check.asked;
    if (!check.credential.isEmpty())
        arguments << (check.asked == QLatin1String("claude") ? QStringLiteral("--claude-token-file")
                                                             : QStringLiteral("--codex-home"))
                  << check.credential;
    arguments << QStringLiteral("--min-blocked-minutes")
              << QString::number(settings_.minBlockedMinutes) << QStringLiteral("--keep")
              << QString::number(settings_.keepCredits) << QStringLiteral("--salvage-hours")
              << QString::number(settings_.salvageHours);
    const auto nowMs = QDateTime::currentMSecsSinceEpoch();
    QStringList keys;
    const auto attempted = attempted_.value(check.machine);
    for (auto it = attempted.cbegin(); it != attempted.cend(); ++it)
        if (it.value() > nowMs)
            keys << it.key();
    if (!keys.isEmpty())
        arguments << QStringLiteral("--attempted") << keys;
    if (withCredentials)
        arguments << QStringLiteral("--claude-credentials-stdin");
    return arguments;
}

void LimitResets::start(const Check& check, const QByteArray& credentials) {
    auto* process = new QProcess(this);
    running_.insert(check.machine, process);
    QByteArray input;
    if (check.machine.isEmpty()) {
        process->setProgram(program_(QStringLiteral("python3")));
        process->setArguments(QStringList{script_path_} + arguments(check, !credentials.isEmpty()));
        if (!credentials.isEmpty())
            input = credentials.simplified() + '\n';
    } else {
        // The helper goes on stdin, so nothing is left on the other machine.
        QStringList words{QStringLiteral("python3"), QStringLiteral("-")};
        for (const auto& word : arguments(check, false))
            words << quoted(word);
        process->setProgram(program_(QStringLiteral("ssh")));
        process->setArguments({QStringLiteral("-o"), QStringLiteral("BatchMode=yes"),
                               QStringLiteral("-o"), QStringLiteral("ConnectTimeout=10"),
                               QStringLiteral("-o"), QStringLiteral("ControlPath=none"),
                               QStringLiteral("-T"), QStringLiteral("--"), check.machine,
                               words.join(QLatin1Char(' '))});
        input = kLimitResetsScript;
    }
    connect(process, &QProcess::finished, this, [this, check, process] { finish(check, process); });
    connect(process, &QProcess::errorOccurred, this,
            [this, check, process](QProcess::ProcessError error) {
                if (error == QProcess::FailedToStart)
                    finish(check, process);
            });
    QTimer::singleShot(kHelperTimeoutMs, process, [process] { process->kill(); });
    process->start();
    process->write(input);
    process->closeWriteChannel();
}

void LimitResets::finish(const Check& check, QProcess* process) {
    if (running_.value(check.machine) != process)
        return;
    running_.remove(check.machine);
    process->deleteLater();
    const auto report = QJsonDocument::fromJson(process->readAllStandardOutput()).object();
    const auto accounts = report.value(QStringLiteral("accounts")).toArray();
    if (accounts.isEmpty()) {
        const auto where = check.machine.isEmpty() ? QStringLiteral("this Mac") : check.machine;
        const auto detail =
            QString::fromUtf8(process->readAllStandardError()).simplified().right(200);
        qWarning().noquote() << "Limit resets: no report from" << where << detail;
        if (!check.asked.isEmpty())
            emit declined(check.machine, check.asked,
                          QStringLiteral("lapis could not ask %1. %2").arg(where, detail));
        return;
    }
    const auto nowMs = QDateTime::currentMSecsSinceEpoch();
    for (const auto& value : accounts)
        note(check, value.toObject(), nowMs);
}

void LimitResets::note(const Check& check, const QJsonObject& account, qint64 nowMs) {
    const auto where = check.machine.isEmpty() ? QStringLiteral("this Mac") : check.machine;
    const auto cli = account.value(QStringLiteral("cli")).toString();
    const auto decision = account.value(QStringLiteral("decision")).toString();
    const auto result = account.value(QStringLiteral("result")).toString();
    const auto key = account.value(QStringLiteral("attempt")).toString();
    // Nothing to reset yet, a throttle, a server error or a lost answer is
    // tried again later; any other answer settles the attempt. Past keys are
    // forgotten once they can no longer come back.
    const bool later =
        result == QLatin1String("nothing_to_reset") || result == QLatin1String("rate_limited") ||
        result == QLatin1String("http_429") || result.startsWith(QLatin1String("http_5")) ||
        result.startsWith(QLatin1String("error: "));
    auto& attempted = attempted_[check.machine];
    attempted.removeIf([nowMs](const auto& entry) { return entry.value() <= nowMs; });
    if (!key.isEmpty() && !result.isEmpty())
        attempted[key] = nowMs + (later ? kRetryLaterMs : kSettledMs);
    if (result == QLatin1String("reset")) {
        const auto email = account.value(QStringLiteral("email")).toString();
        const auto title = account.value(QStringLiteral("credit"))
                               .toObject()
                               .value(QStringLiteral("title"))
                               .toString();
        qInfo().noquote() << "Limit resets: spent" << title << "for" << cli << email << "on"
                          << where
                          << (account.value(QStringLiteral("confirmed")).toBool()
                                  ? "(confirmed)"
                                  : "(not yet reflected in its usage)");
        emit spent(check.machine, cli, email, title, why(decision));
    } else if (cli == check.asked) {
        const auto error = account.value(QStringLiteral("error")).toString();
        emit declined(check.machine, cli,
                      !error.isEmpty()    ? error
                      : !result.isEmpty() ? result
                                          : decision);
    } else if (!result.isEmpty()) {
        qWarning().noquote() << "Limit resets:" << cli << "on" << where << "answered" << result;
    }
}

} // namespace lapis::desktop
