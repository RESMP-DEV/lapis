#include "plan_sign_in.hpp"

#include "plan_sign_in_script.hpp"
#include "platform/updater_process.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTimer>
#include <QUrl>
#include <QUuid>

#include <algorithm>
#include <cstdint>

#include <utility>

namespace lapis::desktop {

QString plan_name_for(const QString& email) {
    const auto at = email.indexOf(QLatin1Char('@'));
    const auto user = at < 0 ? email : email.left(at);
    const auto domain = at < 0 ? QString() : email.mid(at + 1).section(QLatin1Char('.'), 0, 0);
    auto name = domain.isEmpty() ? user : user + QLatin1Char('-') + domain;
    static const QRegularExpression unusable(QStringLiteral("[^A-Za-z0-9._-]"));
    name.replace(unusable, QStringLiteral("-"));
    name.truncate(64);
    return name.isEmpty() ? QStringLiteral("plan") : name;
}

namespace {
constexpr int kCopyTimeoutMs = 30 * 1000;
constexpr qsizetype kMaximumHelperMessage = qsizetype{1024} * 1024;
constexpr qint64 kMaximumTokenBytes = qint64{64} * 1024;

[[nodiscard]] bool isPlanName(const QString& name) {
    static const QRegularExpression valid(QStringLiteral(R"(^[A-Za-z0-9._-]{1,64}$)"));
    return name != QLatin1String(".") && name != QLatin1String("..") &&
           valid.match(name).hasMatch();
}

[[nodiscard]] bool isAnthropicLink(const QString& value) {
    static const QStringList hosts{QStringLiteral("claude.com"), QStringLiteral("claude.ai"),
                                   QStringLiteral("anthropic.com")};
    const QUrl url(value, QUrl::StrictMode);
    if (value.size() > 4096 || !url.isValid() || url.scheme() != QLatin1String("https") ||
        !url.userName().isEmpty() || !url.password().isEmpty() || url.port() != -1 ||
        url.query().isEmpty())
        return false;
    const auto host = url.host();
    const auto allowed = std::any_of(hosts.cbegin(), hosts.cend(), [&](const QString& suffix) {
        return host == suffix || host.endsWith(QLatin1Char('.') + suffix, Qt::CaseInsensitive);
    });
    return allowed && url.path().endsWith(QLatin1String("/oauth/authorize"));
}

// Copy output may echo a token on a hostile or misconfigured host. Keep only
// a short diagnostic, with any credential shape masked.
[[nodiscard]] QString copyReason(const QByteArray& output, const QString& fallback) {
    static const QRegularExpression token(QStringLiteral("sk-ant-oat01-[A-Za-z0-9_-]+"));
    auto text = QString::fromUtf8(output.right(4096));
    text.replace(token, QStringLiteral("[token]"));
    const auto lines = text.split(QRegularExpression(R"(\r?\n)"), Qt::SkipEmptyParts);
    for (auto line = lines.rbegin(); line != lines.rend(); ++line) {
        const auto trimmed = line->trimmed();
        if (!trimmed.isEmpty())
            return trimmed.left(300);
    }
    return fallback;
}
} // namespace

PlanSignIn::PlanSignIn(Hooks hooks, Places places, QObject* parent)
    : QObject(parent), hooks_(std::move(hooks)), places_(std::move(places)) {}

PlanSignIn::~PlanSignIn() {
    stopSignIn();
    stopCopies();
    // QObject destroys the owned UpdaterProcesses; their guards also cover
    // application shutdown before asynchronous cleanup callbacks run.
}

QStringList PlanSignIn::machines() const {
    if (plan_.isEmpty())
        return {};
    return QStringList{tr("this Mac")} + reached_;
}

QString PlanSignIn::pendingToken() const { return pending_token_; }

void PlanSignIn::set(const QString& state, const QString& message) {
    state_ = state;
    message_ = message;
    emit changed();
}

void PlanSignIn::fail(const QString& why) {
    stopSignIn();
    QFile::remove(pendingToken());
    set(QStringLiteral("failed"), copyReason(why.toUtf8(), tr("Sign-in failed")));
}

void PlanSignIn::stopSignIn() {
    auto* process = process_.data();
    process_ = nullptr;
    if (!process)
        return;
    const auto pending = pendingToken();
    QObject::disconnect(process, nullptr, this, nullptr);
    const auto cleanup = [process, pending] {
        process->whenStopped([process, pending] {
            QFile::remove(pending);
            process->deleteLater();
        });
    };
    if (process->state() == QProcess::NotRunning) {
        cleanup();
    } else {
        connect(process, &QProcess::finished, process, cleanup);
        process->terminate();
        QTimer::singleShot(3000, process, [process] { process->stopGroup(); });
    }
}

void PlanSignIn::stopCopies() {
    for (const auto& copy : std::as_const(copying_))
        if (copy.process)
            copy.process->whenStopped([process = copy.process] {
                if (process)
                    process->deleteLater();
            });
    copying_.clear();
    copyReasons_.clear();
}

void PlanSignIn::start() {
    cancel();
    ++attempt_;
    link_.clear();
    plan_.clear();
    output_.clear();
    helper_error_.clear();
    helper_bytes_ = 0;
    pending_token_ = QDir(places_.accounts)
                         .filePath(QStringLiteral("claude/.signing-in-%1.token")
                                       .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    email_.clear();
    emailSubmitted_ = false;
    reached_.clear();
    unreached_.clear();
    copyReasons_.clear();
    const auto python = hooks_.program(QStringLiteral("python3"));
    const auto claude = hooks_.program(QStringLiteral("claude"));
    if (claude.isEmpty())
        return fail(tr("Claude Code is not installed on this Mac."));
    if (python.isEmpty())
        return fail(tr("python3 is not on this Mac."));
    QDir().mkpath(QFileInfo(places_.helper).absolutePath());
    QSaveFile script(places_.helper);
    if (!script.open(QIODevice::WriteOnly) || script.write(kPlanSignInScript) < 0 ||
        !script.commit())
        return fail(tr("lapis could not write its sign-in helper: %1").arg(script.errorString()));
    QFile::setPermissions(places_.helper, QFile::ReadOwner | QFile::WriteOwner);
    auto* process = new UpdaterProcess(this);
    process_ = process;
    process->setProgram(python);
    process->setArguments({places_.helper, QStringLiteral("--token-file"), pendingToken(),
                           QStringLiteral("--claude"), claude});
    process->setProcessChannelMode(QProcess::SeparateChannels);
    connect(process, &QProcess::readyReadStandardOutput, this, [this, process] {
        if (process == process_)
            read();
    });
    connect(process, &QProcess::readyReadStandardError, this, [this, process] {
        const auto bytes = process->readAllStandardError();
        if (process == process_) {
            helper_error_ = (helper_error_ + bytes).right(4096);
            helper_bytes_ += bytes.size();
            if (helper_bytes_ > kMaximumHelperMessage)
                fail(tr("The sign-in helper produced too much output."));
        }
    });
    connect(process, &QProcess::finished, this, [this, process] {
        if (process == process_) {
            ended();
            process->whenStopped([process] { process->deleteLater(); });
        }
    });
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
        if (process == process_ && error == QProcess::FailedToStart)
            fail(tr("lapis could not start its sign-in helper."));
    });
    set(QStringLiteral("starting"));
    QTimer::singleShot(16 * 60 * 1000, process, [this, process] {
        if (process == process_)
            fail(tr("The sign-in helper exceeded its deadline."));
    });
    process->start();
}

void PlanSignIn::read() {
    if (!process_)
        return;
    const auto incoming = process_->readAllStandardOutput();
    helper_bytes_ += incoming.size();
    output_ += incoming;
    if (helper_bytes_ > kMaximumHelperMessage) {
        fail(tr("The sign-in helper produced too much output."));
        return;
    }
    qsizetype end = 0;
    while ((end = output_.indexOf('\n')) >= 0) {
        if (state_ != QLatin1String("starting") && state_ != QLatin1String("waiting"))
            return;
        const auto document = QJsonDocument::fromJson(output_.left(end));
        output_.remove(0, end + 1);
        if (!document.isObject())
            return fail(tr("The sign-in helper sent malformed output."));
        const auto message = document.object();
        const auto linkValue = message.value(QStringLiteral("link"));
        if (linkValue.isString()) {
            const auto link = linkValue.toString();
            if (!isAnthropicLink(link))
                return fail(tr("The sign-in helper sent an unexpected link."));
            link_ = link;
            hooks_.copy(link_);
            hooks_.open(link_);
            set(QStringLiteral("waiting"));
        } else if (const auto signedIn = message.value(QStringLiteral("signedIn"));
                   signedIn.isBool()) {
            if (!signedIn.toBool())
                return fail(tr("The sign-in helper sent malformed output."));
            set(QStringLiteral("signedIn"));
            finish();
            return;
        } else if (const auto error = message.value(QStringLiteral("error")).toString();
                   !error.isEmpty()) {
            fail(error);
        } else {
            return fail(tr("The sign-in helper sent malformed output."));
        }
    }
}

void PlanSignIn::ended() {
    read();
    process_ = nullptr;
    if (state_ == QLatin1String("starting") || state_ == QLatin1String("waiting"))
        fail(copyReason(helper_error_, tr("The sign-in ended before any account signed in.")));
}

void PlanSignIn::setEmail(const QString& email) {
    if (state_ != QLatin1String("starting") && state_ != QLatin1String("waiting") &&
        state_ != QLatin1String("signedIn"))
        return;
    email_ = email.trimmed().toLower();
    emailSubmitted_ = true;
    if (state_ == QLatin1String("signedIn"))
        finish();
}

void PlanSignIn::copyLink() {
    if (!link_.isEmpty())
        hooks_.copy(link_);
}

void PlanSignIn::openLink() {
    if (!link_.isEmpty())
        hooks_.open(link_);
}

void PlanSignIn::finish() {
    static const QRegularExpression address(QStringLiteral(R"(^[^@\s]+@[^@\s]+\.[^@\s]+$)"));
    static const QRegularExpression tokenFormat(
        QStringLiteral(R"(^sk-ant-oat01-[A-Za-z0-9_-]{20,}$)"));
    if (!emailSubmitted_ || !address.match(email_).hasMatch()) {
        set(QStringLiteral("signedIn"), tr("Signed in. Which email did you sign in with?"));
        return;
    }
    QFile pending(pendingToken());
    QByteArray token;
    if (pending.open(QIODevice::ReadOnly) && pending.size() <= kMaximumTokenBytes) {
        token = pending.read(kMaximumTokenBytes + 1).trimmed();
        pending.close();
    }
    if (token.isEmpty() || token.size() > kMaximumTokenBytes ||
        !tokenFormat.match(QString::fromUtf8(token)).hasMatch())
        return fail(tr("The sign-in helper did not keep a usable token."));
    QString reason;
    QString prepared;
    const auto prepare = [this, &token, &prepared](const QString& name, QString* why) {
        if (!isPlanName(name)) {
            *why = tr("lapis cannot use this plan name.");
            return false;
        }
        const auto kept =
            QDir(places_.accounts).filePath(QStringLiteral("claude/%1.token").arg(name));
        QSaveFile keeper(kept);
        if (!keeper.open(QIODevice::WriteOnly) || keeper.write(token + '\n') != token.size() + 1 ||
            !keeper.setPermissions(QFile::ReadOwner | QFile::WriteOwner) || !keeper.commit()) {
            *why = tr("lapis could not keep the token for %1: %2").arg(name, keeper.errorString());
            return false;
        }
        prepared = name;
        return true;
    };
    const auto name = hooks_.record(email_, {}, {}, prepare, &reason);
    if (name.isEmpty() || prepared != name)
        return fail(reason.isEmpty() ? tr("The plan credential was not committed.") : reason);
    QFile::remove(pendingToken());
    plan_ = name;
    spread(token);
    state_ = QStringLiteral("done");
    report();
}

// The token goes to each ssh host on stdin, never on a command line, into
// the same owner-only file the agents' launch reads there.
void PlanSignIn::spread(const QByteArray& token) {
    const auto ssh = hooks_.program(QStringLiteral("ssh"));
    if (token.isEmpty() || ssh.isEmpty() || !hooks_.machines)
        return;
    Q_ASSERT(isPlanName(plan_));
    const auto target =
        QStringLiteral("umask 077 && mkdir -p ~/.lapis/accounts/claude && "
                       "chmod 700 ~/.lapis/accounts ~/.lapis/accounts/claude "
                       "&& tmp=$(mktemp ~/.lapis/accounts/claude/.%1.XXXXXX) "
                       "|| exit\n"
                       "trap 'rm -f \"$tmp\"' EXIT\n"
                       "trap 'exit 1' HUP INT TERM\n"
                       "cat > \"$tmp\" && chmod 600 \"$tmp\" && "
                       "test \"$(wc -c < \"$tmp\")\" -eq %2 && "
                       "mv -f \"$tmp\" \"$HOME/.lapis/accounts/claude/%1.token\"\n"
                       "rc=$?; rm -f \"$tmp\"; trap - EXIT HUP INT TERM; exit \"$rc\"")
            .arg(plan_)
            .arg(token.size() + 1);
    const auto destinations = hooks_.machines(email_);
    constexpr qsizetype max_copies = 64;
    qsizetype admitted = 0;
    for (const auto& machine : destinations) {
        if (admitted >= max_copies) {
            unreached_ << tr("Additional destinations exceed the 64-copy limit");
            break;
        }
        if (machine.isEmpty() || machine.startsWith(QLatin1Char('-')) || copying_.contains(machine))
            continue;
        ++admitted;
        copyTo(machine, ssh, target, token);
    }
}
void PlanSignIn::copyTo(const QString& machine, const QString& ssh, const QString& target,
                        const QByteArray& token) {
    auto* process = new UpdaterProcess(this);
    copying_.insert(machine, {.process = process,
                              .email = email_,
                              .plan = plan_,
                              .machine = machine,
                              .output = {},
                              .attempt = attempt_});
    process->setProgram(ssh);
    process->setArguments({QStringLiteral("-o"), QStringLiteral("BatchMode=yes"),
                           QStringLiteral("-o"), QStringLiteral("ConnectTimeout=10"),
                           QStringLiteral("-o"), QStringLiteral("ControlPath=none"),
                           QStringLiteral("-T"), QStringLiteral("--"), machine, target});
    process->setProcessChannelMode(QProcess::MergedChannels);
    connect(process, &QProcess::readyReadStandardOutput, this, [this, machine, process] {
        const auto bytes = process->readAllStandardOutput();
        auto copy = copying_.find(machine);
        if (copy != copying_.end() && copy->process == process && copy->attempt == attempt_)
            copy->output = (copy->output + bytes).right(qsizetype{16} * 1024);
    });
    const auto done = [this, machine, process](bool ok) {
        const auto copy = copying_.value(machine);
        if (copy.process != process || copy.attempt != attempt_)
            return;
        copying_.remove(machine);
        process->whenStopped([process] { process->deleteLater(); });
        QString reason;
        if (ok && hooks_.record(copy.email, machine, copy.plan, {}, &reason) == copy.plan) {
            reached_ << machine;
        } else {
            const auto why = reason.isEmpty() ? copyReason(copy.output, QString()) : reason;
            unreached_ << machine;
            copyReasons_[machine] = why.isEmpty() ? tr("ssh exited unsuccessfully") : why;
        }
        report();
    };
    connect(process, &QProcess::finished, this, [done](int code, QProcess::ExitStatus status) {
        done(status == QProcess::NormalExit && code == 0);
    });
    connect(process, &QProcess::errorOccurred, this, [done](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            done(false);
    });
    QTimer::singleShot(kCopyTimeoutMs, process, [process] { process->stopGroup(); });
    process->start();
    process->write(token + '\n');
    process->closeWriteChannel();
}

void PlanSignIn::report() {
    auto text = tr("%1 can be used on %2.").arg(plan_, machines().join(QStringLiteral(", ")));
    if (!copying_.isEmpty())
        text +=
            QLatin1Char(' ') + tr("Copying it to %n more machine(s)…", "", int(copying_.size()));
    else if (!unreached_.isEmpty()) {
        QStringList named;
        for (const auto& machine : std::as_const(unreached_))
            named << (copyReasons_.contains(machine)
                          ? tr("%1 (%2)").arg(machine, copyReasons_.value(machine))
                          : machine);
        text += QLatin1Char(' ') + tr("Not reachable: %1.").arg(named.join(QStringLiteral(", ")));
    }
    message_ = std::move(text);
    emit changed();
}

void PlanSignIn::cancel() {
    stopSignIn();
    if (state_ != QLatin1String("done"))
        QFile::remove(pendingToken());
    stopCopies();
    if (state_ != QLatin1String("idle"))
        set(QStringLiteral("idle"));
}

} // namespace lapis::desktop
