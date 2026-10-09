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
#include <QRandomGenerator>
#include <QSaveFile>
#include <QTimer>
#include <QUuid>
#include <lapis/session/terminal.hpp>
#include <memory>

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace lapis::desktop {
namespace {
struct Similarity {
    double value{};
    bool bounded{};
};
constexpr int kContextTimeoutMs = 60 * 1000;
constexpr int kPredictTimeoutMs = 180 * 1000;
constexpr qint64 kHourMs = qint64{60} * 60 * 1000;
constexpr int kScreenChars = 6000;
constexpr qsizetype kCompared = 2000;
constexpr qsizetype kExactSimilarityLimit = 256;
// The log's record format: offer ids and seen/used/withdrawn events.
constexpr int kLogVersion = 2;
constexpr qsizetype kHelperOutputLimit = qsizetype{1024} * 1024;
constexpr qsizetype kHelperErrorLimit = qsizetype{64} * 1024;
constexpr qint64 kLogLimit = qint64{4} * 1024 * 1024;
// The agent's newest reply in the helper's context, which a guess answers;
// clipped to NextPrompt::said_limit without splitting a surrogate pair.
QString last_reply(const QJsonObject& context) {
    const auto turns = context.value(QStringLiteral("turns")).toArray();
    for (auto index = turns.size(); index > 0; --index) {
        const auto object = turns.at(index - 1).toObject();
        if (object.value(QStringLiteral("role")).toString() != QLatin1String("agent"))
            continue;
        auto text = object.value(QStringLiteral("text")).toString().trimmed();
        if (text.size() > NextPrompt::said_limit) {
            qsizetype end = NextPrompt::said_limit;
            if (text.at(end - 1).isHighSurrogate())
                --end;
            text.truncate(end);
        }
        return text;
    }
    return {};
}
struct HelperResult {
    QByteArray output;
    qsizetype error_bytes{};
    bool oversized{};
    bool timed_out{};
    void drain(UpdaterProcess& process) {
        const auto bytes = process.readAllStandardOutput();
        if (output.size() + bytes.size() > kHelperOutputLimit)
            oversized = true;
        else if (!oversized)
            output.append(bytes);
        error_bytes += process.readAllStandardError().size();
        if (error_bytes > kHelperErrorLimit)
            oversized = true;
        if (oversized)
            process.stopGroup();
    }
    [[nodiscard]] QString failure(const QProcess& process, const QJsonDocument& document,
                                  const QJsonParseError& error) const {
        if (timed_out)
            return QStringLiteral("timeout");
        if (oversized)
            return QStringLiteral("output too large");
        if (process.error() == QProcess::FailedToStart)
            return QStringLiteral("helper unavailable");
        if (error.error != QJsonParseError::NoError)
            return QStringLiteral("invalid helper JSON");
        const auto answer = document.object();
        if (!document.isObject() || answer.isEmpty() || process.exitCode() != 0)
            return QStringLiteral("helper failed");
        if (answer.contains(QStringLiteral("error"))) {
            const auto message = answer.value(QStringLiteral("error")).toString();
            return message.isEmpty() ? QStringLiteral("helper failed") : message;
        }
        return {};
    }
};
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
    if (const auto share = object.value(QStringLiteral("experiment")); share.isDouble())
        settings.experimentShare = std::clamp(share.toDouble(), 0.0, 1.0);
    if (const auto models = object.value(QStringLiteral("experimentModels")); models.isArray()) {
        settings.experimentModels.clear();
        for (const auto& model : models.toArray())
            if (const auto name = model.toString().trimmed(); !name.isEmpty())
                settings.experimentModels.append(name.left(100));
    }
    settings.localEndpoint =
        object.value(QStringLiteral("localEndpoint")).toString().trimmed().left(200);
    settings.localModel = object.value(QStringLiteral("localModel")).toString().trimmed().left(100);
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
    auto text = rows.join(QLatin1Char('\n')).right(kScreenChars);
    if (!text.isEmpty() && text.front().isLowSurrogate())
        text.remove(0, 1);
    return text;
}

NextPrompt::NextPrompt(Lookup lookup, Agents agents, Program program, const Files& files,
                       QObject* parent)
    : QObject(parent), lookup_(std::move(lookup)), agents_(std::move(agents)),
      program_(std::move(program)),
      script_path_(QDir(files.folder).filePath(QStringLiteral("next_prompt.py"))),
      log_path_(files.log),
      // Offer ids are unique across launches: the log outlives any one.
      run_(QUuid::createUuid().toString(QUuid::WithoutBraces)) {
    QDir().mkpath(files.folder, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
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
            run.process->stopGroup();
}

void NextPrompt::setSettings(NextPromptSettings settings) {
    const bool was = settings_.automatic;
    settings_ = std::move(settings);
    if (!settings_.automatic) {
        for (const auto& id : offers_.keys())
            withdraw(id, Withdrawal::off);
        for (const auto& id : restoring_.keys())
            dropRestored(id, "off");
        owed_.clear();
        awaiting_.clear();
        emit stateChanged();
        for (const auto& run : std::as_const(running_))
            if (run.process)
                run.process->stopGroup();
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

void NextPrompt::turnFinished(const QString& id) { run(id, false); }

void NextPrompt::focused(const QString& id) {
    // A show during the context fetch arrives before deferred_ is filled. An
    // already-focused agent gets no further focusChanged, so window activation
    // reports it here too.
    if (!id.isEmpty())
        pending_show_.insert(id);
    if (deferred_.remove(id))
        run(id, true);
}

double NextPrompt::confidence(const QString& id) const { return offers_.value(id).p; }

void NextPrompt::run(const QString& id, bool force) {
    if (!settings_.automatic)
        return;
    // A repeat is judged by the model's own verdict for the last guess: a
    // guessed-past turn stays quiet, and re-running this prediction replaces
    // that verdict once the model answers.
    const auto judged_for = [this, &id](const QString& attention) { emit judged(id, attention); };
    if (const auto shown = offers_.constFind(id); shown != offers_.cend())
        previous_.insert(id, {shown->conversation, judged_for, shown->turn, shown->seen_ms != 0});
    deferred_.remove(id);
    owed_.remove(id);
    dropRestored(id, "new_turn");
    withdraw(id, Withdrawal::next_turn);
    const auto agent = lookup_(id);
    if (!agent || (agent->cli != QLatin1String("claude") && agent->cli != QLatin1String("codex"))) {
        emit judged(id, {});
        return;
    }
    if (auto old = running_.take(id); old.process)
        old.process->stopGroup();
    if (!budgetAvailable(id)) {
        emit judged(id, {});
        return;
    }
    const auto generation = ++generation_;
    running_.insert(id, {generation, *agent, {}});
    emit stateChanged();
    runContext(id, generation, *agent, Stage::context,
               [this, id, generation, force](const QJsonObject& context) {
                   settle(id, context);
                   // A show can arrive while this fetch is in flight; consume it
                   // here so the repeat is guessed rather than deferred again.
                   const bool shown_during_run = pending_show_.remove(id);
                   // A repeat: the person has sent nothing since the last guess
                   // and never saw it. Guess again when they show this agent.
                   const auto last = previous_.value(id);
                   if (!force && !last.seen && last.turn >= 0 &&
                       last.conversation ==
                           context.value(QStringLiteral("conversation")).toString() &&
                       last.turn == context.value(QStringLiteral("turn")).toInt()) {
                       if (shown_during_run) {
                           predict(id, generation, context);
                           return;
                       }
                       running_.remove(id);
                       deferred_.insert(id);
                       record({{QStringLiteral("event"), QStringLiteral("skipped")},
                               {QStringLiteral("agent"), id},
                               {QStringLiteral("reason"), QStringLiteral("unseen_repeat")}});
                       last.attention(id);
                       return;
                   }
                   predict(id, generation, context);
               });
}

QStringList NextPrompt::contextWords(const QString& id, const Agent& agent) const {
    QStringList words{QStringLiteral("context"),
                      QStringLiteral("--cli"),
                      agent.cli,
                      QStringLiteral("--folder"),
                      agent.folder,
                      QStringLiteral("--conversation"),
                      agent.conversation};
    if (const auto waiting = awaiting_.constFind(id); waiting != awaiting_.cend())
        words << QStringLiteral("--answered") << QString::number(waiting->offer.turn);
    return words;
}

void NextPrompt::runContext(const QString& id, quint64 generation, const Agent& agent, Stage stage,
                            const std::function<void(const QJsonObject&)>& done) {
    const auto words = contextWords(id, agent);
    if (agent.machine.isEmpty()) {
        start(id, generation, program_(QStringLiteral("python3")),
              QStringList{script_path_} + words, {}, stage, done);
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
           agent.machine, remote.join(QLatin1Char(' '))},
          kNextPromptScript, stage, done);
}

bool NextPrompt::budgetAvailable(const QString& id) {
    const auto now = clock_.elapsed();
    while (!started_.empty() && now - started_.front().at >= kHourMs)
        started_.pop_front();
    if (std::cmp_less(started_.size(), settings_.maxPerHour))
        return true;
    record({{QStringLiteral("event"), QStringLiteral("skipped")},
            {QStringLiteral("agent"), id},
            {QStringLiteral("reason"), QStringLiteral("hourly_cap")},
            {QStringLiteral("max_per_hour"), settings_.maxPerHour}});
    return false;
}

void NextPrompt::start(const QString& id, quint64 generation, const QString& program,
                       const QStringList& arguments, const QByteArray& input, Stage stage,
                       const std::function<void(const QJsonObject&)>& done) {
    auto* process = new UpdaterProcess(this);
    running_[id].process = process;
    process->setProgram(program);
    process->setArguments(arguments);
    const auto result = std::make_shared<HelperResult>();
    const auto drain = [process, result] { result->drain(*process); };
    connect(process, &QProcess::readyReadStandardOutput, process, drain);
    connect(process, &QProcess::readyReadStandardError, process, drain);
    const auto finish = [this, id, generation, process, stage, done, result] {
        result->drain(*process);
        process->deleteLater();
        if (stage == Stage::predict && process->error() == QProcess::FailedToStart)
            std::erase_if(started_, [generation](const Attempt& attempt) {
                return attempt.generation == generation;
            });
        if (!current(id, generation) || running_.value(id).process != process)
            return;
        QJsonParseError error{};
        const auto document = QJsonDocument::fromJson(result->output, &error);
        const auto why = result->failure(*process, document, error);
        if (!why.isEmpty()) {
            const auto agent = running_.take(id).agent;
            emit stateChanged();
            if (stage == Stage::verify)
                dropRestored(id, "unverified");
            else
                failed(id, agent, stage, why);
            return;
        }
        done(document.object());
    };
    connect(process, &QProcess::finished, this, finish);
    connect(process, &QProcess::errorOccurred, this, [finish](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            finish();
    });
    QTimer::singleShot(stage == Stage::predict ? kPredictTimeoutMs : kContextTimeoutMs, process,
                       [process, result] {
                           result->timed_out = true;
                           process->stopGroup();
                       });
    process->start();
    process->write(input);
    process->closeWriteChannel();
}

void NextPrompt::predict(const QString& id, quint64 generation, const QJsonObject& context) {
    const auto claude = program_(QStringLiteral("claude"));
    if (claude.isEmpty()) {
        failed(id, running_.take(id).agent, Stage::predict,
               QStringLiteral("no Claude Code CLI on this Mac"));
        emit stateChanged();
        return;
    }
    // Context extraction is concurrent. Reserve here too, otherwise several
    // contexts admitted below the cap could all launch after it was reached.
    if (!budgetAvailable(id)) {
        running_.remove(id);
        emit stateChanged();
        emit judged(id, {});
        return;
    }
    started_.push_back({clock_.elapsed(), generation});
    const auto agent = running_.value(id).agent;
    QJsonObject about{{QStringLiteral("title"), agent.title},
                      {QStringLiteral("folder"), agent.folder},
                      {QStringLiteral("cli"), agent.cli},
                      {QStringLiteral("machine"), agent.machine},
                      {QStringLiteral("category"), agent.category}};
    const QJsonObject bundle{{QStringLiteral("agent"), about},
                             {QStringLiteral("screen"), agent.screen},
                             {QStringLiteral("context"), context},
                             {QStringLiteral("agents"), agents_()},
                             {QStringLiteral("time"), QDateTime::currentDateTime().toString(
                                                          QStringLiteral("ddd h:mm ap"))}};
    // Most guesses come from the chosen model; a share goes to an experiment
    // arm, evenly among the other models and the local endpoint.
    QString arm = QStringLiteral("control");
    QString model = settings_.model;
    QString endpoint;
    QStringList arms;
    for (const auto& other : settings_.experimentModels)
        if (other != settings_.model)
            arms << other;
    if (!settings_.localEndpoint.isEmpty() && !settings_.localModel.isEmpty())
        arms << QStringLiteral("local");
    if (!arms.isEmpty() &&
        QRandomGenerator::global()->generateDouble() < settings_.experimentShare) {
        const auto& pick = arms.at(QRandomGenerator::global()->bounded(arms.size()));
        arm = pick == QLatin1String("local") ? pick : QStringLiteral("model");
        model = pick == QLatin1String("local") ? settings_.localModel : pick;
        if (pick == QLatin1String("local"))
            endpoint = settings_.localEndpoint;
    }
    QStringList arguments{script_path_, QStringLiteral("predict"),  QStringLiteral("--model"),
                          model,        QStringLiteral("--claude"), claude};
    if (!settings_.effort.isEmpty() && endpoint.isEmpty() && model == settings_.model)
        arguments << QStringLiteral("--effort") << settings_.effort;
    if (!endpoint.isEmpty())
        arguments << QStringLiteral("--endpoint") << endpoint << QStringLiteral("--style")
                  << QStringLiteral("personal");
    start(id, generation, program_(QStringLiteral("python3")), arguments, line(bundle),
          Stage::predict, [this, id, agent, context, arm, model](const QJsonObject& answer) {
              running_.remove(id);
              offer(id, agent, context, answer, arm, model);
              emit stateChanged();
          });
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void NextPrompt::offer(const QString& id, const Agent& agent, const QJsonObject& context,
                       // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
                       const QJsonObject& answer, const QString& arm, const QString& model) {
    const auto candidates = answer.value(QStringLiteral("candidates")).toArray();
    const auto top = candidates.isEmpty() ? QJsonObject() : candidates.first().toObject();
    const auto text = top.value(QStringLiteral("text")).toString();
    const auto probability = top.value(QStringLiteral("p"));
    // The helper always emits a numeric p (0.0 for unscored candidates, flagged
    // `scored: false`), so isDouble() only rejects replies that skipped that
    // normalization. A reported 0.0 still counts at the default; whether the
    // model scored the top guess is recorded as top_scored below.
    const bool usable_probability = probability.isDouble();
    const bool shown = !text.isEmpty() && usable_probability &&
                       probability.toDouble() >= settings_.minConfidence && settings_.automatic;
    const Offer made{QStringLiteral("%1:%2.%3").arg(id, run_).arg(++offers_made_),
                     text,
                     context.value(QStringLiteral("conversation")).toString(),
                     context.value(QStringLiteral("turn")).toInt(),
                     0,
                     last_reply(context),
                     probability.toDouble()};
    auto event = about(made, id);
    event.insert(QStringLiteral("event"), QStringLiteral("predicted"));
    event.insert(QStringLiteral("machine"), agent.machine);
    event.insert(QStringLiteral("cli"), agent.cli);
    event.insert(QStringLiteral("model"), model);
    event.insert(QStringLiteral("arm"), arm);
    event.insert(QStringLiteral("attention"), answer.value(QStringLiteral("attention")));
    event.insert(QStringLiteral("category"), answer.value(QStringLiteral("category")));
    event.insert(QStringLiteral("candidates"), candidates);
    event.insert(QStringLiteral("top_scored"), top.value(QStringLiteral("scored")).toBool());
    event.insert(QStringLiteral("shown"), shown);
    event.insert(QStringLiteral("min_confidence"), settings_.minConfidence);
    event.insert(QStringLiteral("ms"), answer.value(QStringLiteral("ms")));
    record(event);
    emit judged(id, answer.value(QStringLiteral("attention")).toString());
    if (!shown)
        return;
    const auto judged_for = [this, &id](const QString& attention) { emit judged(id, attention); };
    previous_.insert(id, {made.conversation, judged_for, made.turn, made.seen_ms != 0});
    offers_.insert(id, made);
    // A guess already typed in stays the one its prompt is compared with.
    if (!awaiting_.value(id).filled)
        awaiting_.insert(id, {made, false, false});
    ++revision_;
    emit changed();
}

QString NextPrompt::suggestion(const QString& id) const { return offers_.value(id).text; }

// A completed attempt that came to nothing records its stage and reason.
// Superseded or disabled work is cancelled before producing an offer.
void NextPrompt::failed(const QString& id, const Agent& agent, Stage stage, const QString& why) {
    const QStringList known{
        QStringLiteral("no transcript"),           QStringLiteral("no Claude Code CLI on this Mac"),
        QStringLiteral("invalid conversation id"), QStringLiteral("timeout"),
        QStringLiteral("output too large"),        QStringLiteral("invalid helper JSON"),
        QStringLiteral("helper unavailable")};
    const auto reason = known.contains(why) ? why : QStringLiteral("helper failed");
    record({{QStringLiteral("event"), QStringLiteral("failed")},
            {QStringLiteral("agent"), id},
            {QStringLiteral("machine"), agent.machine},
            {QStringLiteral("cli"), agent.cli},
            {QStringLiteral("stage"),
             stage == Stage::predict ? QStringLiteral("predict") : QStringLiteral("context")},
            {QStringLiteral("error"), reason}});
    emit judged(id, {});
}

QVariantMap NextPrompt::readyAgents() const {
    QVariantMap ready;
    for (auto offer = offers_.cbegin(); offer != offers_.cend(); ++offer)
        ready.insert(offer.key(), offer->seen_ms != 0);
    return ready;
}

QString NextPrompt::offerKey(const QString& id) const { return offers_.value(id).key; }

QJsonObject NextPrompt::offerState(const QString& id) const {
    const auto offer = offers_.constFind(id);
    if (offer == offers_.cend())
        return {};
    return {{QStringLiteral("key"), offer->key},
            {QStringLiteral("text"), offer->text},
            {QStringLiteral("seen"), offer->seen_ms != 0},
            {QStringLiteral("said"), offer->said}};
}

QJsonObject NextPrompt::about(const Offer& offer, const QString& id) {
    return {{QStringLiteral("offer"), offer.key},
            {QStringLiteral("agent"), id},
            {QStringLiteral("conversation"), offer.conversation},
            {QStringLiteral("turn"), offer.turn}};
}

void NextPrompt::seen(const QString& id) {
    seenOffer({{QStringLiteral("session"), id}, {QStringLiteral("offer"), offers_.value(id).key}});
}
void NextPrompt::seenOffer(const QVariantMap& identity) {
    const auto id = identity.value(QStringLiteral("session")).toString();
    const auto expectedKey = identity.value(QStringLiteral("offer")).toString();
    const auto offer = offers_.find(id);
    if (offer == offers_.end() || (expectedKey.isEmpty() || offer->key != expectedKey) ||
        offer->seen_ms != 0)
        return;
    offer->seen_ms = QDateTime::currentMSecsSinceEpoch();
    if (auto previous = previous_.find(id); previous != previous_.end())
        previous->seen = true;
    auto event = about(*offer, id);
    event.insert(QStringLiteral("event"), QStringLiteral("seen"));
    record(event);
    emit stateChanged();
    emit seenChanged();
}

void NextPrompt::used(const QString& id, bool sent, int typed_first, const QString& expectedKey) {
    // A second Tab sent the guess Tab had typed: its offer is already used.
    if (sent) {
        const auto waiting = awaiting_.find(id);
        if (waiting == awaiting_.end() || !waiting->filled ||
            (!expectedKey.isEmpty() && waiting->offer.key != expectedKey))
            return;
        waiting->tab_sent = true;
        emit stateChanged();
        auto event = about(waiting->offer, id);
        event.insert(QStringLiteral("event"), QStringLiteral("used"));
        event.insert(QStringLiteral("sent"), true);
        event.insert(QStringLiteral("typed_first"), typed_first);
        event.insert(QStringLiteral("ms_after_seen"),
                     waiting->offer.seen_ms == 0
                         ? -1
                         : QDateTime::currentMSecsSinceEpoch() - waiting->offer.seen_ms);
        record(event);
        if (auto previous = previous_.find(id); previous != previous_.end())
            previous->seen = true;
        return;
    }
    const auto offer = offers_.value(id);
    if (offer.text.isEmpty() || (!expectedKey.isEmpty() && offer.key != expectedKey))
        return;
    auto event = about(offer, id);
    event.insert(QStringLiteral("event"), QStringLiteral("used"));
    event.insert(QStringLiteral("sent"), sent);
    event.insert(QStringLiteral("typed_first"), typed_first);
    event.insert(QStringLiteral("ms_after_seen"),
                 offer.seen_ms == 0 ? -1 : QDateTime::currentMSecsSinceEpoch() - offer.seen_ms);
    record(event);
    if (auto previous = previous_.find(id); previous != previous_.end())
        previous->seen = true;
    // Taking a newer offer over a still-waiting guess makes that newer offer
    // the one its eventual prompt must be compared with.
    awaiting_.insert(id, {offer, true, false});
    offers_.remove(id);
    ++revision_;
    emit changed();
    emit stateChanged();
}

void NextPrompt::withdraw(const QString& id, Withdrawal why) {
    const auto offer = offers_.take(id);
    if (offer.text.isEmpty())
        return;
    recordWithdrawn(offer, id, why == Withdrawal::off ? "off" : "new_turn");
    ++revision_;
    emit changed();
    emit stateChanged();
}

void NextPrompt::recordWithdrawn(const Offer& offer, const QString& id, const char* reason) const {
    auto event = about(offer, id);
    event.insert(QStringLiteral("event"), QStringLiteral("withdrawn"));
    event.insert(QStringLiteral("reason"), QLatin1String(reason));
    event.insert(QStringLiteral("seen"), offer.seen_ms != 0);
    record(event);
}

namespace {
// How alike two prompts are: 1 minus their edit distance over the longer
// length, so 1 is the same text and 0 nothing in common. Short prompts use the
// exact distance. Longer prompts, which settle in a GUI callback, use only a
// conservative shared-prefix/suffix score and are marked bounded.
Similarity similarity(const QString& guess, const QString& sent) {
    const auto a = guess.trimmed().left(kCompared);
    const auto b = sent.trimmed().left(kCompared);
    if (a.isEmpty() && b.isEmpty())
        return {1.0, false};
    const auto longest = std::max(a.size(), b.size());
    if (longest > kExactSimilarityLimit) {
        qsizetype prefix = 0;
        while (prefix < a.size() && prefix < b.size() && a[prefix] == b[prefix])
            ++prefix;
        qsizetype suffix = 0;
        while (suffix < a.size() - prefix && suffix < b.size() - prefix &&
               a[a.size() - suffix - 1] == b[b.size() - suffix - 1])
            ++suffix;
        const auto shared = std::min(prefix + suffix, std::min(a.size(), b.size()));
        return {double(shared) / double(longest), true};
    }
    std::vector<qsizetype> previous(static_cast<std::size_t>(b.size()) + 1);
    std::vector<qsizetype> row(previous.size());
    for (qsizetype j = 0; j <= b.size(); ++j)
        previous[static_cast<std::size_t>(j)] = j;
    for (qsizetype i = 1; i <= a.size(); ++i) {
        row[0] = i;
        for (qsizetype j = 1; j <= b.size(); ++j) {
            const auto at = static_cast<std::size_t>(j);
            const qsizetype replace = previous[at - 1] + (a[i - 1] == b[j - 1] ? 0 : 1);
            row[at] = std::min({previous[at] + 1, row[at - 1] + 1, replace});
        }
        std::swap(previous, row);
    }
    const auto distance = previous[static_cast<std::size_t>(b.size())];
    return {1.0 - double(distance) / double(longest), false};
}
} // namespace

// Once the conversation holds the prompt sent after the last offer, record it
// beside the guess: used as is, edited after Tab, or typed instead. A turn the
// agent started on its own leaves the offer waiting; a new conversation (as
// after /clear) drops it.
void NextPrompt::settle(const QString& id, const QJsonObject& context) {
    const auto waiting = awaiting_.constFind(id);
    if (waiting == awaiting_.cend())
        return;
    const auto conversation = context.value(QStringLiteral("conversation")).toString();
    if (!waiting->offer.conversation.isEmpty() && conversation != waiting->offer.conversation) {
        awaiting_.erase(waiting);
        emit stateChanged();
        return;
    }
    const auto answered = context.value(QStringLiteral("answered")).toObject();
    if (answered.value(QStringLiteral("turn")).toInt(-1) != waiting->offer.turn)
        return;
    const auto sent = answered.value(QStringLiteral("text")).toString();
    const auto& guess = waiting->offer.text;
    const auto compared_guess = guess.trimmed().left(kCompared);
    const auto compared_sent = sent.trimmed().left(kCompared);
    const bool exact = sent.trimmed() == guess.trimmed();
    const auto score = similarity(guess, sent);
    const bool prefix_bounded = compared_guess == compared_sent && !exact;
    auto event = about(waiting->offer, id);
    event.insert(QStringLiteral("event"), QStringLiteral("outcome"));
    event.insert(QStringLiteral("filled"), waiting->filled);
    event.insert(QStringLiteral("tab_sent"), waiting->tab_sent);
    event.insert(QStringLiteral("result"), exact             ? QStringLiteral("as_offered")
                                           : waiting->filled ? QStringLiteral("edited")
                                                             : QStringLiteral("own"));
    event.insert(QStringLiteral("similarity"), std::round(score.value * 1000.0) / 1000.0);
    event.insert(QStringLiteral("similarity_bounded"), prefix_bounded || score.bounded);
    event.insert(QStringLiteral("sent_text"), sent.left(4000));
    awaiting_.erase(waiting);
    record(event);
    emit stateChanged();
}

void NextPrompt::record(QJsonObject event) const {
    if (log_path_.isEmpty())
        return;
    event.insert(QStringLiteral("v"), kLogVersion);
    event.insert(QStringLiteral("t"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    const auto encoded = line(event);
    if (encoded.size() > kHelperOutputLimit) {
        qWarning() << "Next prompt: log record exceeds its byte limit";
        return;
    }
    QFile file(log_path_);
    if (file.exists() && !file.setPermissions(QFile::ReadOwner | QFile::WriteOwner)) {
        qWarning() << "Next prompt: cannot make the log private";
        return;
    }
    if (file.exists() && file.size() + encoded.size() > kLogLimit) {
        const auto previous = log_path_ + QStringLiteral(".1");
        QFile::remove(previous);
        if (!file.rename(previous)) {
            qWarning() << "Next prompt: cannot rotate log";
            return;
        }
        file.setFileName(log_path_);
    }
    const bool created = !file.exists();
    if (created)
        QDir().mkpath(QFileInfo(log_path_).absolutePath(),
                      QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    if (!file.open(QIODevice::Append | QIODevice::WriteOnly,
                   QFile::ReadOwner | QFile::WriteOwner)) {
        qWarning() << "Next prompt: cannot write the log:" << file.errorString();
        return;
    }
    // Owner-only however it was created: it holds screens and conversations.
    if (!file.setPermissions(QFile::ReadOwner | QFile::WriteOwner)) {
        qWarning() << "Next prompt: cannot make the log private";
        return;
    }
    file.write(encoded);
}

// Saved state ------------------------------------------------------------------

namespace {
// Bounds on what is saved: agents per map, characters per guess, and all the
// guesses' text together, so the state file stays small.
constexpr qsizetype kSavedAgents = 128;
constexpr qsizetype kSavedText = 8000;
constexpr qsizetype kSavedTextTotal = qsizetype{256} * 1024;
bool chats(const QString& cli) {
    return cli == QLatin1String("claude") || cli == QLatin1String("codex");
}
} // namespace

QJsonObject NextPrompt::encodeOffer(const Offer& offer, qsizetype& budget) const {
    if (offer.key.size() > 400 || offer.conversation.size() > 200 || offer.text.isEmpty() ||
        offer.text.size() > kSavedText || offer.text.size() + offer.said.size() > budget)
        return {};
    budget -= offer.text.size() + offer.said.size();
    return {{QStringLiteral("key"), offer.key},
            {QStringLiteral("text"), offer.text},
            {QStringLiteral("conversation"), offer.conversation},
            {QStringLiteral("turn"), offer.turn},
            {QStringLiteral("seenMs"), offer.seen_ms},
            {QStringLiteral("said"), offer.said}};
}

QJsonObject NextPrompt::savedOffers(qsizetype& budget) const {
    QJsonObject saved;
    for (const auto* map : {&offers_, &restoring_})
        for (auto entry = map->cbegin(); entry != map->cend(); ++entry)
            if (!entry.key().isEmpty() && entry.key().size() <= 200 && saved.size() < kSavedAgents)
                if (const auto value = encodeOffer(entry.value(), budget); !value.isEmpty())
                    saved.insert(entry.key(), value);
    return saved;
}

QJsonObject NextPrompt::savedAwaiting(qsizetype& budget) const {
    QJsonObject saved;
    for (auto entry = awaiting_.cbegin(); entry != awaiting_.cend(); ++entry) {
        if (entry.key().isEmpty() || entry.key().size() > 200)
            continue;
        auto value = encodeOffer(entry->offer, budget);
        if (value.isEmpty() || saved.size() >= kSavedAgents)
            continue;
        value.insert(QStringLiteral("filled"), entry->filled);
        value.insert(QStringLiteral("tabSent"), entry->tab_sent);
        saved.insert(entry.key(), value);
    }
    return saved;
}

QJsonObject NextPrompt::savedOwed() const {
    QJsonObject saved;
    for (auto entry = running_.cbegin(); entry != running_.cend(); ++entry)
        if (!entry->verifying && !entry.key().isEmpty() && entry.key().size() <= 200 &&
            entry->agent.conversation.size() <= 200 && saved.size() < kSavedAgents)
            saved.insert(entry.key(),
                         QJsonObject{{QStringLiteral("conversation"), entry->agent.conversation}});
    for (auto entry = owed_.cbegin(); entry != owed_.cend(); ++entry)
        if (!entry.key().isEmpty() && entry.key().size() <= 200 && entry.value().size() <= 200 &&
            !saved.contains(entry.key()) && saved.size() < kSavedAgents)
            saved.insert(entry.key(), QJsonObject{{QStringLiteral("conversation"), entry.value()}});
    return saved;
}

QJsonObject NextPrompt::saveState() const {
    qsizetype budget = kSavedTextTotal;
    return {{QStringLiteral("offers"), savedOffers(budget)},
            {QStringLiteral("awaiting"), savedAwaiting(budget)},
            {QStringLiteral("owed"), savedOwed()}};
}

void NextPrompt::restoreState(const QJsonObject& state) {
    if (!settings_.automatic)
        return;
    // Awaiting first: a restored offer's check asks for its outcome too.
    const bool awaiting = restoreAwaiting(state.value(QStringLiteral("awaiting")).toObject());
    const bool offers = restoreOffers(state.value(QStringLiteral("offers")).toObject());
    const bool owed = restoreOwed(state.value(QStringLiteral("owed")).toObject());
    if (awaiting || offers || owed)
        emit stateChanged();
}

auto NextPrompt::savedOffer(const QString& id, const QJsonValue& value) -> std::optional<Offer> {
    const auto object = value.toObject();
    Offer offer;
    offer.key = object.value(QStringLiteral("key")).toString();
    offer.text = object.value(QStringLiteral("text")).toString();
    offer.conversation = object.value(QStringLiteral("conversation")).toString();
    offer.turn = object.value(QStringLiteral("turn")).toInt(-1);
    offer.seen_ms = static_cast<qint64>(object.value(QStringLiteral("seenMs")).toDouble(-1));
    offer.said = object.value(QStringLiteral("said")).toString().left(said_limit);
    if (id.isEmpty() || id.size() > 200 || !offer.key.startsWith(id + QLatin1Char(':')) ||
        offer.key.size() > 400 || offer.text.isEmpty() || offer.text.size() > kSavedText ||
        offer.conversation.size() > 200 || offer.turn < 0 || offer.seen_ms < 0)
        return std::nullopt;
    return offer;
}

// The agent still exists, runs a CLI lapis guesses for, and has not moved to
// another conversation (as after /clear) as far as lapis knows yet. Either
// conversation may be empty because that side does not know it yet; the helper
// verifies the actual conversation before a restored offer is shown.
auto NextPrompt::restorable(const QString& id, QStringView conversation) const
    -> std::optional<Agent> {
    auto agent = lookup_(id);
    if (!agent || !chats(agent->cli) ||
        (!agent->conversation.isEmpty() && !conversation.isEmpty() &&
         agent->conversation != conversation))
        return std::nullopt;
    return agent;
}

bool NextPrompt::restoreAwaiting(const QJsonObject& awaiting) {
    bool changed = false;
    for (auto entry = awaiting.constBegin(); entry != awaiting.constEnd(); ++entry) {
        const auto offer = savedOffer(entry.key(), entry.value());
        if (!offer || awaiting_.contains(entry.key()) ||
            !restorable(entry.key(), offer->conversation)) {
            changed = true; // the rejected entry must leave the saved state too
            continue;
        }
        const auto object = entry.value().toObject();
        Awaiting restored;
        restored.offer = *offer;
        restored.filled = object.value(QStringLiteral("filled")).toBool();
        restored.tab_sent = object.value(QStringLiteral("tabSent")).toBool();
        awaiting_.insert(entry.key(), restored);
        changed = true;
    }
    return changed;
}

bool NextPrompt::restoreOffers(const QJsonObject& offers) {
    bool changed = false;
    for (auto entry = offers.constBegin(); entry != offers.constEnd(); ++entry) {
        const auto& id = entry.key();
        const auto offer = savedOffer(id, entry.value());
        if (!offer || offers_.contains(id) || restoring_.contains(id) || running_.contains(id)) {
            changed = true; // malformed or duplicate saved entries are pruned
            continue;
        }
        // lapis may not know the saved conversation; the helper's verify step
        // checks the actual conversation and turn before the offer is shown.
        const auto known = lookup_(id);
        const char* reason = "gone";
        if (known != std::nullopt) {
            if (!chats(known->cli))
                reason = "unsupported";
            else if (!known->conversation.isEmpty() && !offer->conversation.isEmpty() &&
                     known->conversation != offer->conversation)
                reason = "moved";
            else {
                verify(id, *known, *offer);
                changed = true;
                continue;
            }
        }
        recordWithdrawn(*offer, id, reason);
        changed = true;
    }
    return changed;
}

bool NextPrompt::restoreOwed(const QJsonObject& owed) {
    bool changed = false;
    for (auto entry = owed.constBegin(); entry != owed.constEnd(); ++entry) {
        const auto& id = entry.key();
        const auto conversation =
            entry.value().toObject().value(QStringLiteral("conversation")).toString();
        if (id.isEmpty() || id.size() > 200 || conversation.size() > 200 || owed_.contains(id) ||
            offers_.contains(id) || restoring_.contains(id) || running_.contains(id) ||
            !restorable(id, conversation)) {
            changed = true; // the rejected entry must leave the saved state too
            continue;
        }
        owed_.insert(id, conversation);
        changed = true;
        QTimer::singleShot(0, this, [this, id] { resumeOwed(id, 0); });
    }
    return changed;
}

void NextPrompt::verify(const QString& id, const Agent& agent, const Offer& offer) {
    const auto generation = ++generation_;
    running_.insert(id, {generation, agent, {}, true});
    restoring_.insert(id, offer);
    runContext(id, generation, agent, Stage::verify, [this, id](const QJsonObject& context) {
        // What was sent after an earlier guess may have reached the
        // conversation while no window was watching.
        settle(id, context);
        confirm(id, context);
    });
}

void NextPrompt::confirm(const QString& id, const QJsonObject& context) {
    running_.remove(id);
    const auto offer = restoring_.value(id);
    if (offer.text.isEmpty())
        return;
    const auto conversation = context.value(QStringLiteral("conversation")).toString();
    if (conversation != offer.conversation) {
        dropRestored(id, "stale");
        return;
    }
    if (context.value(QStringLiteral("turn")).toInt(-1) != offer.turn) {
        dropRestored(id, "new_turn");
        return;
    }
    restoring_.remove(id);
    offers_.insert(id, offer);
    ++revision_;
    emit changed();
    emit stateChanged();
}

void NextPrompt::dropRestored(const QString& id, const char* reason) {
    const auto offer = restoring_.take(id);
    if (offer.text.isEmpty())
        return;
    recordWithdrawn(offer, id, reason);
    emit stateChanged();
}

void NextPrompt::resumeOwed(const QString& id, int tries) {
    if (!owed_.contains(id) || !settings_.automatic)
        return;
    const auto agent = lookup_(id);
    const auto conversation = owed_.value(id);
    if (!agent || !chats(agent->cli) ||
        (!agent->conversation.isEmpty() && !conversation.isEmpty() &&
         agent->conversation != conversation)) {
        owed_.remove(id);
        emit stateChanged();
        return;
    }
    // The guess reads the agent's screen, which the service sends again
    // shortly after the window reattaches. A screen that has still not
    // arrived leaves the guess owed; predicting from context alone would
    // invent a prompt for an unknown screen.
    if (agent->screen.isEmpty()) {
        if (tries < owed_tries_)
            QTimer::singleShot(owed_retry_ms_, this,
                               [this, id, tries] { resumeOwed(id, tries + 1); });
        // After the retry budget, the entry stays owed rather than degrading
        // the guess; the next window restart can retry it with a screen.
        return;
    }
    if (running_.contains(id) || offers_.contains(id)) {
        owed_.remove(id);
        emit stateChanged();
        return;
    }
    turnFinished(id);
}

void NextPrompt::setOwedRetryForTesting(int tries, int retryMs) {
    owed_tries_ = std::max(0, tries);
    owed_retry_ms_ = std::max(0, retryMs);
}

} // namespace lapis::desktop
