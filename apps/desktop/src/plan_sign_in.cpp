#include "plan_sign_in.hpp"

#include "plan_sign_in_script.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTimer>

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
}

PlanSignIn::PlanSignIn(Hooks hooks, Places places, QObject* parent)
    : QObject(parent), hooks_(std::move(hooks)), places_(std::move(places)) {}

PlanSignIn::~PlanSignIn() {
    if (process_)
        process_->kill();
    for (const auto& copy : std::as_const(copying_))
        if (copy)
            copy->kill();
}

QStringList PlanSignIn::machines() const {
    if (plan_.isEmpty())
        return {};
    return QStringList{tr("this Mac")} + reached_;
}

QString PlanSignIn::pendingToken() const {
    return QDir(places_.accounts).filePath(QStringLiteral("claude/.signing-in.token"));
}

void PlanSignIn::set(const QString& state, const QString& message) {
    state_ = state;
    message_ = message;
    emit changed();
}

void PlanSignIn::fail(const QString& why) {
    QFile::remove(pendingToken());
    set(QStringLiteral("failed"), why);
}

void PlanSignIn::start() {
    cancel();
    link_.clear();
    plan_.clear();
    output_.clear();
    reached_.clear();
    unreached_.clear();
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
    auto* process = new QProcess(this);
    process_ = process;
    process->setProgram(python);
    process->setArguments({places_.helper, QStringLiteral("--token-file"), pendingToken(),
                           QStringLiteral("--claude"), claude});
    process->setProcessChannelMode(QProcess::SeparateChannels);
    connect(process, &QProcess::readyReadStandardOutput, this, &PlanSignIn::read);
    connect(process, &QProcess::finished, this, [this, process] {
        if (process == process_)
            ended();
        process->deleteLater();
    });
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
        if (process == process_ && error == QProcess::FailedToStart)
            fail(tr("lapis could not start its sign-in helper."));
    });
    set(QStringLiteral("starting"));
    process->start();
}

void PlanSignIn::read() {
    if (!process_)
        return;
    output_ += process_->readAllStandardOutput();
    qsizetype end = 0;
    while ((end = output_.indexOf('\n')) >= 0) {
        const auto message = QJsonDocument::fromJson(output_.left(end)).object();
        output_.remove(0, end + 1);
        if (const auto link = message.value(QStringLiteral("link")).toString();
            link.startsWith(QLatin1String("https://"))) {
            link_ = link;
            hooks_.copy(link_);
            hooks_.open(link_);
            set(QStringLiteral("waiting"));
        } else if (message.value(QStringLiteral("signedIn")).toBool()) {
            set(QStringLiteral("signedIn"));
            finish();
        } else if (const auto error = message.value(QStringLiteral("error")).toString();
                   !error.isEmpty()) {
            fail(error);
        }
    }
}

void PlanSignIn::ended() {
    read();
    process_ = nullptr;
    if (state_ == QLatin1String("starting") || state_ == QLatin1String("waiting"))
        fail(tr("The sign-in ended before any account signed in."));
}

void PlanSignIn::setEmail(const QString& email) {
    email_ = email.trimmed().toLower();
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
    if (!address.match(email_).hasMatch()) {
        set(QStringLiteral("signedIn"), tr("Signed in. Which email did you sign in with?"));
        return;
    }
    QString reason;
    const auto name = hooks_.record(email_, {}, &reason);
    if (name.isEmpty())
        return fail(reason);
    const auto kept = QDir(places_.accounts).filePath(QStringLiteral("claude/%1.token").arg(name));
    QFile::remove(kept);
    if (!QFile::rename(pendingToken(), kept))
        return fail(tr("lapis could not keep the token for %1.").arg(name));
    plan_ = name;
    QFile token(kept);
    spread(token.open(QIODevice::ReadOnly) ? token.read(8192).trimmed() : QByteArray());
    state_ = QStringLiteral("done");
    report();
}

// The token goes to each ssh host on stdin, never on a command line, into
// the same owner-only file the agents' launch reads there.
void PlanSignIn::spread(const QByteArray& token) {
    const auto ssh = hooks_.program(QStringLiteral("ssh"));
    if (token.isEmpty() || ssh.isEmpty() || !hooks_.machines)
        return;
    const auto target = QStringLiteral("umask 077 && mkdir -p ~/.lapis/accounts/claude && "
                                       "chmod 700 ~/.lapis/accounts ~/.lapis/accounts/claude "
                                       "&& cat > ~/.lapis/accounts/claude/%1.token")
                            .arg(plan_);
    for (const auto& machine : hooks_.machines()) {
        if (machine.isEmpty() || machine.startsWith(QLatin1Char('-')) || copying_.contains(machine))
            continue;
        auto* process = new QProcess(this);
        copying_.insert(machine, process);
        process->setProgram(ssh);
        process->setArguments({QStringLiteral("-o"), QStringLiteral("BatchMode=yes"),
                               QStringLiteral("-o"), QStringLiteral("ConnectTimeout=10"),
                               QStringLiteral("-o"), QStringLiteral("ControlPath=none"),
                               QStringLiteral("-T"), QStringLiteral("--"), machine, target});
        process->setProcessChannelMode(QProcess::MergedChannels);
        const auto done = [this, machine, process](bool ok) {
            if (copying_.value(machine) != process)
                return;
            copying_.remove(machine);
            process->deleteLater();
            QString reason;
            if (ok && !hooks_.record(email_, machine, &reason).isEmpty())
                reached_ << machine;
            else
                unreached_ << machine;
            report();
        };
        connect(process, &QProcess::finished, this,
                [process, done](int code, QProcess::ExitStatus status) {
                    Q_UNUSED(process);
                    done(status == QProcess::NormalExit && code == 0);
                });
        connect(process, &QProcess::errorOccurred, this, [done](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart)
                done(false);
        });
        QTimer::singleShot(kCopyTimeoutMs, process, [process] { process->kill(); });
        process->start();
        process->write(token + '\n');
        process->closeWriteChannel();
    }
}

void PlanSignIn::report() {
    auto text = tr("%1 can be used on %2.").arg(plan_, machines().join(QStringLiteral(", ")));
    if (!copying_.isEmpty())
        text +=
            QLatin1Char(' ') + tr("Copying it to %n more machine(s)…", "", int(copying_.size()));
    else if (!unreached_.isEmpty())
        text +=
            QLatin1Char(' ') + tr("Not reachable: %1.").arg(unreached_.join(QStringLiteral(", ")));
    message_ = std::move(text);
    emit changed();
}

void PlanSignIn::cancel() {
    if (process_) {
        // The helper ends Claude Code's sign-in with it on SIGTERM.
        const QPointer<QProcess> process = process_;
        process_ = nullptr;
        process->terminate();
        QTimer::singleShot(3000, process, [process] { process->kill(); });
    }
    if (state_ != QLatin1String("done"))
        QFile::remove(pendingToken());
    if (state_ != QLatin1String("idle") && state_ != QLatin1String("done") &&
        state_ != QLatin1String("failed"))
        set(QStringLiteral("idle"));
}

} // namespace lapis::desktop
