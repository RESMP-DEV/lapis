#include "next_prompt.hpp"

#include "next_prompt_script.hpp"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <QTimer>
#include <lapis/session/terminal.hpp>

#include <algorithm>
#include <utility>

namespace lapis::desktop {
namespace {
constexpr int kContextTimeoutMs = 60 * 1000;
constexpr int kPredictTimeoutMs = 180 * 1000;
constexpr qint64 kHourMs = qint64{60} * 60 * 1000;
constexpr int kScreenChars = 6000;
QString quoted(const QString& word) {
    return QLatin1Char('\'') + QString(word).replace(QLatin1Char('\''), QStringLiteral("'\\''")) +
           QLatin1Char('\'');
}
QByteArray line(const QJsonObject& object) {
    return QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
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
      log_path_(files.log) {
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
    for (const auto& run : std::as_const(running_))
        if (run.process)
            run.process->kill();
}

void NextPrompt::setSettings(NextPromptSettings settings) {
    settings_ = std::move(settings);
    if (!settings_.automatic)
        for (const auto& id : offers_.keys())
            withdraw(id);
}

bool NextPrompt::current(const QString& id, quint64 generation) const {
    const auto run = running_.constFind(id);
    return run != running_.cend() && run->generation == generation;
}

void NextPrompt::turnFinished(const QString& id) {
    if (!settings_.automatic)
        return;
    withdraw(id);
    const auto agent = lookup_(id);
    if (!agent || (agent->cli != QLatin1String("claude") && agent->cli != QLatin1String("codex")))
        return;
    const auto now = clock_.elapsed();
    while (!started_.empty() && now - started_.front() > kHourMs)
        started_.pop_front();
    if (std::cmp_greater_equal(started_.size(), settings_.maxPerHour)) {
        qInfo().noquote() << "Next prompt: skipped; at" << settings_.maxPerHour << "an hour";
        return;
    }
    started_.push_back(now);
    if (auto old = running_.take(id); old.process)
        old.process->kill();
    const auto generation = ++generation_;
    running_.insert(id, {generation, *agent, {}});
    QStringList words{
        QStringLiteral("context"),  QStringLiteral("--cli"), agent->cli,
        QStringLiteral("--folder"), agent->folder,           QStringLiteral("--conversation"),
        agent->conversation};
    const auto done = [this, id, generation](const QJsonObject& context) {
        predict(id, generation, context);
    };
    if (agent->machine.isEmpty()) {
        start(id, generation, program_(QStringLiteral("python3")),
              QStringList{script_path_} + words, {}, kContextTimeoutMs, done);
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
          kNextPromptScript, kContextTimeoutMs, done);
}

void NextPrompt::start(const QString& id, quint64 generation, const QString& program,
                       const QStringList& arguments, const QByteArray& input, int timeout_ms,
                       const std::function<void(const QJsonObject&)>& done) {
    auto* process = new QProcess(this);
    running_[id].process = process;
    process->setProgram(program);
    process->setArguments(arguments);
    const auto finish = [this, id, generation, process, done] {
        process->deleteLater();
        if (!current(id, generation) || running_.value(id).process != process)
            return;
        const auto answer = QJsonDocument::fromJson(process->readAllStandardOutput()).object();
        if (answer.isEmpty() || answer.contains(QStringLiteral("error"))) {
            running_.remove(id);
            const auto why =
                answer.isEmpty()
                    ? QString::fromUtf8(process->readAllStandardError()).simplified().right(200)
                    : answer.value(QStringLiteral("error")).toString();
            qInfo().noquote() << "Next prompt: none for" << id << why;
            return;
        }
        done(answer);
    };
    connect(process, &QProcess::finished, this, finish);
    connect(process, &QProcess::errorOccurred, this, [finish](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            finish();
    });
    QTimer::singleShot(timeout_ms, process, [process] { process->kill(); });
    process->start();
    process->write(input);
    process->closeWriteChannel();
}

void NextPrompt::predict(const QString& id, quint64 generation, const QJsonObject& context) {
    const auto claude = program_(QStringLiteral("claude"));
    if (claude.isEmpty()) {
        running_.remove(id);
        qInfo() << "Next prompt: no Claude Code CLI on this Mac";
        return;
    }
    const auto agent = running_.value(id).agent;
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
          kPredictTimeoutMs, [this, id, agent, context](const QJsonObject& answer) {
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
    const auto conversation = context.value(QStringLiteral("conversation")).toString();
    const int turn = context.value(QStringLiteral("turn")).toInt();
    record({{QStringLiteral("event"), QStringLiteral("predicted")},
            {QStringLiteral("agent"), id},
            {QStringLiteral("machine"), agent.machine},
            {QStringLiteral("cli"), agent.cli},
            {QStringLiteral("conversation"), conversation},
            {QStringLiteral("turn"), turn},
            {QStringLiteral("model"), settings_.model},
            {QStringLiteral("category"), answer.value(QStringLiteral("category"))},
            {QStringLiteral("candidates"), candidates},
            {QStringLiteral("shown"), shown},
            {QStringLiteral("ms"), answer.value(QStringLiteral("ms"))}});
    if (!shown)
        return;
    offers_.insert(id, {text, conversation, turn});
    ++revision_;
    emit changed();
}

QString NextPrompt::suggestion(const QString& id) const { return offers_.value(id).text; }

void NextPrompt::used(const QString& id, bool sent) {
    const auto offer = offers_.value(id);
    if (offer.text.isEmpty())
        return;
    record({{QStringLiteral("event"), QStringLiteral("used")},
            {QStringLiteral("agent"), id},
            {QStringLiteral("conversation"), offer.conversation},
            {QStringLiteral("turn"), offer.turn},
            {QStringLiteral("sent"), sent}});
    withdraw(id);
}

void NextPrompt::dismiss(const QString& id) {
    const auto offer = offers_.value(id);
    if (offer.text.isEmpty())
        return;
    record({{QStringLiteral("event"), QStringLiteral("dismissed")},
            {QStringLiteral("agent"), id},
            {QStringLiteral("conversation"), offer.conversation},
            {QStringLiteral("turn"), offer.turn}});
    withdraw(id);
}

void NextPrompt::withdraw(const QString& id) {
    if (offers_.remove(id) == 0)
        return;
    ++revision_;
    emit changed();
}

void NextPrompt::record(QJsonObject event) const {
    if (log_path_.isEmpty())
        return;
    event.insert(QStringLiteral("t"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    QFile file(log_path_);
    const bool created = !file.exists();
    if (created)
        QDir().mkpath(QFileInfo(log_path_).absolutePath());
    if (!file.open(QIODevice::Append | QIODevice::WriteOnly)) {
        qWarning() << "Next prompt: cannot write the log:" << file.errorString();
        return;
    }
    if (created)
        file.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
    file.write(line(event));
}

} // namespace lapis::desktop
