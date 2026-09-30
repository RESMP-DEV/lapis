#include "next_prompt.hpp"

#include "next_prompt_script.hpp"
#include "platform/updater_process.hpp"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSaveFile>
#include <QTimer>
#include <QUuid>
#include <lapis/session/terminal.hpp>

#include <algorithm>
#include <memory>
#include <utility>

namespace lapis::desktop {
namespace {
constexpr int kContextTimeoutMs = 60 * 1000;
constexpr int kPredictTimeoutMs = 180 * 1000;
constexpr qint64 kHourMs = qint64{60} * 60 * 1000;
constexpr int kScreenChars = 6000;
constexpr qint64 kMaxHelperOutputBytes = 1024 * 1024;
constexpr qint64 kMaxHelperErrorBytes = 64 * 1024;
constexpr qint64 kMaxLogBytes = 4 * 1024 * 1024;
// The log's record format: offer ids and seen/used/withdrawn events.
constexpr int kLogVersion = 2;
QString quoted(const QString& word) {
    return QLatin1Char('\'') + QString(word).replace(QLatin1Char('\''), QStringLiteral("'\\''")) +
           QLatin1Char('\'');
}
QByteArray line(const QJsonObject& object) {
    return QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
}
bool make_private_directory(const QString& path) {
    if (path.isEmpty() || (!QDir(path).exists() && !QDir().mkpath(path)))
        return false;
    return QFile(path).setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
}
// Helper failures are bounded log categories, not model output or paths.
QString failure_reason(bool prediction, const QString& raw) {
    static const QStringList known{
        QStringLiteral("no transcript"),          QStringLiteral("invalid conversation id"),
        QStringLiteral("invalid folder"),         QStringLiteral("invalid time"),
        QStringLiteral("invalid model response"), QStringLiteral("model call failed"),
    };
    const auto reason = raw.simplified().left(100);
    if (known.contains(reason))
        return reason;
    return prediction ? QStringLiteral("model call failed") : QStringLiteral("context failed");
}
} // namespace

NextPromptSettings parse_next_prompt(const QJsonValue& value) {
    NextPromptSettings settings;
    const auto object = value.toObject();
    if (object.value(QStringLiteral("auto")).isBool())
        settings.automatic = object.value(QStringLiteral("auto")).toBool();
    if (const auto model = object.value(QStringLiteral("model")).toString().trimmed();
        !model.isEmpty())
        settings.model = model.left(100);
    settings.effort = object.value(QStringLiteral("effort")).toString().trimmed().left(20);
    if (const auto p = object.value(QStringLiteral("minConfidence")); p.isDouble())
        settings.minConfidence = std::clamp(p.toDouble(), 0.0, 1.0);
    if (const auto cap = object.value(QStringLiteral("maxPerHour")); cap.isDouble())
        settings.maxPerHour = std::max(0, cap.toInt());
    return settings;
}

QString terminal_screen_text(const session::TerminalSnapshot& snapshot) {
    QStringList rows;
    const std::size_t columns = snapshot.size.columns;
    for (std::size_t row = 0; row < snapshot.size.rows; ++row) {
        QString text;
        for (std::size_t column = 0; column < columns; ++column) {
            const auto index = row * columns + column;
            if (index >= snapshot.cells.size())
                break;
            const auto kind = snapshot.cells[index].kind;
            if (kind == session::CellKind::wide_tail || kind == session::CellKind::wrap_spacer)
                continue;
            const auto cell = snapshot.text(index);
            text += cell.empty()
                        ? QStringLiteral(" ")
                        : QString::fromUcs4(cell.data(), static_cast<qsizetype>(cell.size()));
        }
        while (text.endsWith(QLatin1Char(' ')))
            text.chop(1);
        rows.append(text);
    }
    while (!rows.isEmpty() && rows.last().isEmpty())
        rows.removeLast();
    return rows.join(QLatin1Char('\n')).right(kScreenChars);
}

NextPrompt::NextPrompt(Lookup lookup, Agents agents, Program program, const Files& files,
                       QObject* parent)
    : QObject(parent), lookup_(std::move(lookup)), agents_(std::move(agents)),
      program_(std::move(program)),
      script_path_(QDir(files.folder).filePath(QStringLiteral("next_prompt.py"))),
      log_path_(files.log), launch_id_(QUuid::createUuid().toString(QUuid::WithoutBraces)) {
    if (!make_private_directory(files.folder)) {
        qWarning() << "Next prompt: cannot create the private runtime folder";
        script_path_.clear();
    }
    QSaveFile script(script_path_);
    if (script.open(QIODevice::WriteOnly)) {
        script.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
        script.write(kNextPromptScript);
        if (!script.commit())
            qWarning() << "Next prompt: cannot write the helper:" << script.errorString();
    }
    clock_.start();
}

NextPrompt::~NextPrompt() {
    ++generation_;
    QList<QPointer<QProcess>> processes;
    for (const auto& run : std::as_const(running_))
        processes << run.process;
    running_.clear();
    for (const auto& process : std::as_const(processes)) {
        if (!process)
            continue;
        process->kill();
        process->waitForFinished(1000);
    }
}

void NextPrompt::setSettings(NextPromptSettings settings) {
    const bool was = settings_.automatic;
    settings_ = std::move(settings);
    if (!settings_.automatic) {
        for (const auto& id : offers_.keys())
            withdraw(id, Withdrawal::off);
        ++generation_;
        for (const auto& run : std::as_const(running_))
            if (run.process)
                run.process->kill();
        running_.clear();
    }
    if (was != settings_.automatic) {
        ++revision_;
        emit changed();
    }
}

bool NextPrompt::current(const QString& id, quint64 generation) const {
    const auto run = running_.constFind(id);
    return run != running_.cend() && run->generation == generation;
}

void NextPrompt::turnFinished(const QString& id) {
    if (!settings_.automatic)
        return;
    withdraw(id, Withdrawal::next_turn);
    const auto agent = lookup_(id);
    if (!agent || (agent->cli != QLatin1String("claude") && agent->cli != QLatin1String("codex")))
        return;
    const auto now = clock_.elapsed();
    while (!started_.empty() && now - started_.front() > kHourMs)
        started_.pop_front();
    if (std::cmp_greater_equal(started_.size(), settings_.maxPerHour)) {
        record({{QStringLiteral("event"), QStringLiteral("skipped")},
                {QStringLiteral("agent"), id},
                {QStringLiteral("reason"), QStringLiteral("hourly_cap")},
                {QStringLiteral("max_per_hour"), settings_.maxPerHour}});
        return;
    }
    started_.push_back(now);
    if (auto old = running_.take(id); old.process) {
        superseded(id, old);
        old.process->kill();
    }
    const auto generation = ++generation_;
    running_.insert(id, {generation, *agent, {}, Stage::context, {}});
    QStringList words{
        QStringLiteral("context"),  QStringLiteral("--cli"), agent->cli,
        QStringLiteral("--folder"), agent->folder,           QStringLiteral("--conversation"),
        agent->conversation};
    const auto done = [this, id, generation](const QJsonObject& context) {
        predict(id, generation, context);
    };
    if (agent->machine.isEmpty()) {
        start(id, generation, program_(QStringLiteral("python3")),
              QStringList{script_path_} + words, {}, Stage::context, done);
        return;
    }
    // The helper goes on stdin, so nothing is left on the other machine.
    QStringList remote{QStringLiteral("python3"), QStringLiteral("-")};
    for (const auto& word : words)
        remote << quoted(word);
    start(id, generation, program_(QStringLiteral("ssh")),
          {QStringLiteral("-o"), QStringLiteral("BatchMode=yes"), QStringLiteral("-o"),
           QStringLiteral("ConnectTimeout=10"), QStringLiteral("-o"),
           QStringLiteral("ControlPath=none"), QStringLiteral("-T"), QStringLiteral("--"),
           agent->machine, remote.join(QLatin1Char(' '))},
          kNextPromptScript, Stage::context, done);
}

void NextPrompt::start(const QString& id, quint64 generation, const QString& program,
                       const QStringList& arguments, const QByteArray& input, Stage stage,
                       const std::function<void(const QJsonObject&)>& done) {
    auto* process = new UpdaterProcess(this);
    running_[id].process = process;
    running_[id].stage = stage;
    process->setProgram(program);
    process->setArguments(arguments);
    const auto output = std::make_shared<QByteArray>();
    const auto errors = std::make_shared<QByteArray>();
    const auto output_truncated = std::make_shared<bool>(false);
    const auto timed_out = std::make_shared<bool>(false);
    const auto drain = [process, output, errors, output_truncated] {
        const auto more_output = process->readAllStandardOutput();
        if (!*output_truncated) {
            if (output->size() + more_output.size() > kMaxHelperOutputBytes) {
                *output_truncated = true;
                output->clear();
                process->stopGroup();
            } else {
                output->append(more_output);
            }
        }
        const auto more_errors = process->readAllStandardError();
        if (errors->size() + more_errors.size() <= kMaxHelperErrorBytes)
            errors->append(more_errors);
        else {
            *errors = errors->left(kMaxHelperErrorBytes);
            *output_truncated = true;
            process->stopGroup();
        }
    };
    connect(process, &QProcess::readyReadStandardOutput, process, drain);
    connect(process, &QProcess::readyReadStandardError, process, drain);
    const auto finish = [this, id, generation, process, stage, done, output, errors,
                         output_truncated, timed_out, drain] {
        drain();
        process->deleteLater();
        if (!current(id, generation) || running_.value(id).process != process)
            return;
        QJsonParseError parse_error{};
        const auto document = QJsonDocument::fromJson(*output, &parse_error);
        const auto answer = document.object();
        const bool bad_answer = *output_truncated ||
                                parse_error.error != QJsonParseError::NoError ||
                                document.isNull() || !document.isObject() || answer.isEmpty() ||
                                answer.contains(QStringLiteral("error"));
        if (bad_answer) {
            QString why;
            if (*timed_out)
                why = QStringLiteral("timeout");
            else if (*output_truncated)
                why = QStringLiteral("output too large");
            else if (parse_error.error != QJsonParseError::NoError)
                why = QStringLiteral("invalid helper JSON");
            else if (answer.contains(QStringLiteral("error")))
                why = failure_reason(stage == Stage::predict,
                                     answer.value(QStringLiteral("error")).toString());
            else
                why = process->error() == QProcess::Crashed ? QStringLiteral("process failed")
                                                            : QStringLiteral("no answer");
            failed(id, stage, why);
            running_.remove(id);
            return;
        }
        done(answer);
    };
    connect(process, &QProcess::finished, this, finish);
    connect(process, &QProcess::errorOccurred, this, [finish](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            finish();
    });
    QTimer::singleShot(stage == Stage::predict ? kPredictTimeoutMs : kContextTimeoutMs, process,
                       [process, timed_out] {
                           *timed_out = true;
                           process->stopGroup();
                       });
    process->start();
    process->write(input);
    process->closeWriteChannel();
}

void NextPrompt::predict(const QString& id, quint64 generation, const QJsonObject& context) {
    const auto claude = program_(QStringLiteral("claude"));
    if (claude.isEmpty()) {
        const auto run = running_.value(id);
        failed(id, Stage::predict, QStringLiteral("model call failed"));
        running_.remove(id);
        if (run.process)
            run.process->kill();
        return;
    }
    auto run = running_.value(id);
    run.context = context;
    running_[id].context = context;
    const auto agent = run.agent;
    QJsonObject about{{QStringLiteral("title"), agent.title},
                      {QStringLiteral("cli"), agent.cli},
                      {QStringLiteral("machine"), agent.machine},
                      {QStringLiteral("category"), agent.category}};
    const QJsonObject bundle{{QStringLiteral("agent"), about},
                             {QStringLiteral("screen"), agent.screen},
                             {QStringLiteral("context"), context},
                             {QStringLiteral("agents"), agents_()},
                             {QStringLiteral("time"), QDateTime::currentDateTime().toString(
                                                          QStringLiteral("ddd h:mm ap"))}};
    QStringList arguments{script_path_,    QStringLiteral("predict"),  QStringLiteral("--model"),
                          settings_.model, QStringLiteral("--claude"), claude};
    if (!settings_.effort.isEmpty())
        arguments << QStringLiteral("--effort") << settings_.effort;
    start(id, generation, program_(QStringLiteral("python3")), arguments, line(bundle),
          Stage::predict, [this, id, agent, context](const QJsonObject& answer) {
              running_.remove(id);
              offer(id, agent, context, answer);
          });
}

void NextPrompt::offer(const QString& id, const Agent& agent, const QJsonObject& context,
                       const QJsonObject& answer) {
    const auto candidates = answer.value(QStringLiteral("candidates")).toArray();
    const auto top = candidates.isEmpty() ? QJsonObject() : candidates.first().toObject();
    const auto text = top.value(QStringLiteral("text")).toString();
    const bool shown = !text.isEmpty() &&
                       top.value(QStringLiteral("p")).toDouble() >= settings_.minConfidence &&
                       settings_.automatic;
    const Offer made{QStringLiteral("%1:%2:%3").arg(launch_id_, id).arg(++offers_made_), text,
                     context.value(QStringLiteral("conversation")).toString(),
                     context.value(QStringLiteral("turn")).toInt(), 0};
    auto event = about(made, id);
    event.insert(QStringLiteral("event"), QStringLiteral("predicted"));
    event.insert(QStringLiteral("machine"), agent.machine);
    event.insert(QStringLiteral("cli"), agent.cli);
    event.insert(QStringLiteral("model"), settings_.model);
    event.insert(QStringLiteral("category"), answer.value(QStringLiteral("category")));
    event.insert(QStringLiteral("candidates"), candidates);
    event.insert(QStringLiteral("shown"), shown);
    event.insert(QStringLiteral("min_confidence"), settings_.minConfidence);
    event.insert(QStringLiteral("ms"), answer.value(QStringLiteral("ms")));
    record(event);
    if (!shown)
        return;
    offers_.insert(id, made);
    ++revision_;
    emit changed();
}

QString NextPrompt::suggestion(const QString& id) const { return offers_.value(id).text; }

// A superseded attempt is recorded before its process is killed.
void NextPrompt::superseded(const QString& id, const Run& run) {
    record({{QStringLiteral("event"), QStringLiteral("failed")},
            {QStringLiteral("agent"), id},
            {QStringLiteral("machine"), run.agent.machine},
            {QStringLiteral("cli"), run.agent.cli},
            {QStringLiteral("conversation"), run.context.value(QStringLiteral("conversation"))},
            {QStringLiteral("turn"), run.context.value(QStringLiteral("turn"))},
            {QStringLiteral("stage"),
             run.stage == Stage::predict ? QStringLiteral("predict") : QStringLiteral("context")},
            {QStringLiteral("error"), QStringLiteral("superseded")}});
}

// A prediction that came to nothing is recorded too, with its bounded category.
void NextPrompt::failed(const QString& id, Stage stage, const QString& why) {
    const auto run = running_.constFind(id);
    const auto agent = run != running_.cend() ? std::optional<Agent>{run->agent} : lookup_(id);
    const auto context = run != running_.cend() ? run->context : QJsonObject{};
    record({{QStringLiteral("event"), QStringLiteral("failed")},
            {QStringLiteral("agent"), id},
            {QStringLiteral("conversation"), context.value(QStringLiteral("conversation"))},
            {QStringLiteral("turn"), context.value(QStringLiteral("turn"))},
            {QStringLiteral("machine"), agent ? agent->machine : QString()},
            {QStringLiteral("cli"), agent ? agent->cli : QString()},
            {QStringLiteral("stage"),
             stage == Stage::predict ? QStringLiteral("predict") : QStringLiteral("context")},
            {QStringLiteral("error"), why.left(300)}});
}

QStringList NextPrompt::readyAgents() const { return offers_.keys(); }

QJsonObject NextPrompt::about(const Offer& offer, const QString& id) {
    return {{QStringLiteral("offer"), offer.key},
            {QStringLiteral("agent"), id},
            {QStringLiteral("conversation"), offer.conversation},
            {QStringLiteral("turn"), offer.turn}};
}

void NextPrompt::seen(const QString& id) {
    const auto offer = offers_.find(id);
    if (offer == offers_.end() || offer->seen_ms != 0)
        return;
    offer->seen_ms = QDateTime::currentMSecsSinceEpoch();
    auto event = about(*offer, id);
    event.insert(QStringLiteral("event"), QStringLiteral("seen"));
    record(event);
}

void NextPrompt::used(const QString& id, bool sent, int typed_first) {
    const auto offer = offers_.value(id);
    if (offer.text.isEmpty())
        return;
    auto event = about(offer, id);
    event.insert(QStringLiteral("event"), QStringLiteral("used"));
    event.insert(QStringLiteral("sent"), sent);
    event.insert(QStringLiteral("typed_first"), typed_first);
    event.insert(QStringLiteral("ms_after_seen"),
                 offer.seen_ms == 0 ? -1 : QDateTime::currentMSecsSinceEpoch() - offer.seen_ms);
    record(event);
    offers_.remove(id);
    ++revision_;
    emit changed();
}

void NextPrompt::withdraw(const QString& id, Withdrawal why) {
    const auto offer = offers_.take(id);
    if (offer.text.isEmpty())
        return;
    auto event = about(offer, id);
    event.insert(QStringLiteral("event"), QStringLiteral("withdrawn"));
    event.insert(QStringLiteral("reason"),
                 why == Withdrawal::off ? QStringLiteral("off") : QStringLiteral("new_turn"));
    event.insert(QStringLiteral("seen"), offer.seen_ms != 0);
    record(event);
    ++revision_;
    emit changed();
}

void NextPrompt::record(QJsonObject event) const {
    if (log_path_.isEmpty())
        return;
    event.insert(QStringLiteral("v"), kLogVersion);
    event.insert(QStringLiteral("t"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    QFile file(log_path_);
    if (!make_private_directory(QFileInfo(log_path_).absolutePath())) {
        qWarning() << "Next prompt: cannot create the log folder";
        return;
    }
    if (file.exists() && file.size() >= kMaxLogBytes) {
        const auto backup = log_path_ + QStringLiteral(".1");
        QFile::remove(backup);
        if (!file.rename(backup)) {
            qWarning() << "Next prompt: cannot rotate the log:" << file.errorString();
            return;
        }
        QFile(backup).setPermissions(QFile::ReadOwner | QFile::WriteOwner);
        file.setFileName(log_path_);
    }
    if (!file.open(QIODevice::Append | QIODevice::WriteOnly)) {
        qWarning() << "Next prompt: cannot write the log:" << file.errorString();
        return;
    }
    file.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
    file.write(line(event));
}

} // namespace lapis::desktop
