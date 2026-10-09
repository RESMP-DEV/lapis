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
        awaiting_.clear();
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
    if (deferred_.remove(id))
        run(id, true);
}

double NextPrompt::confidence(const QString& id) const { return offers_.value(id).p; }

void NextPrompt::run(const QString& id, bool force) {
    if (!settings_.automatic)
        return;
    if (const auto shown = offers_.constFind(id); shown != offers_.cend())
        previous_.insert(id, {shown->conversation, shown->turn, shown->seen_ms != 0});
    deferred_.remove(id);
    withdraw(id, Withdrawal::next_turn);
    const auto agent = lookup_(id);
    if (!agent || (agent->cli != QLatin1String("claude") && agent->cli != QLatin1String("codex")))
        return;
    if (auto old = running_.take(id); old.process)
        old.process->stopGroup();
    if (!budgetAvailable(id))
        return;
    const auto generation = ++generation_;
    running_.insert(id, {generation, *agent, {}});
    QStringList words{
        QStringLiteral("context"),  QStringLiteral("--cli"), agent->cli,
        QStringLiteral("--folder"), agent->folder,           QStringLiteral("--conversation"),
        agent->conversation};
    if (const auto waiting = awaiting_.constFind(id); waiting != awaiting_.cend())
        words << QStringLiteral("--answered") << QString::number(waiting->offer.turn);
    const auto done = [this, id, generation, force](const QJsonObject& context) {
        settle(id, context);
        // A repeat: the person has sent nothing since the last guess and never
        // saw it. Guess again when they show this agent, not now.
        const auto last = previous_.value(id);
        if (!force && !last.seen && last.turn >= 0 &&
            last.conversation == context.value(QStringLiteral("conversation")).toString() &&
            last.turn == context.value(QStringLiteral("turn")).toInt()) {
            running_.remove(id);
            deferred_.insert(id);
            record({{QStringLiteral("event"), QStringLiteral("skipped")},
                    {QStringLiteral("agent"), id},
                    {QStringLiteral("reason"), QStringLiteral("unseen_repeat")}});
            emit judged(id, {});
            return;
        }
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
        return;
    }
    // Context extraction is concurrent. Reserve here too, otherwise several
    // contexts admitted below the cap could all launch after it was reached.
    if (!budgetAvailable(id)) {
        running_.remove(id);
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
        const auto pick = arms.at(QRandomGenerator::global()->bounded(arms.size()));
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
          });
}

void NextPrompt::offer(const QString& id, const Agent& agent, const QJsonObject& context,
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
    auto event = about(*offer, id);
    event.insert(QStringLiteral("event"), QStringLiteral("seen"));
    record(event);
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
        auto event = about(waiting->offer, id);
        event.insert(QStringLiteral("event"), QStringLiteral("used"));
        event.insert(QStringLiteral("sent"), true);
        event.insert(QStringLiteral("typed_first"), typed_first);
        event.insert(QStringLiteral("ms_after_seen"),
                     waiting->offer.seen_ms == 0
                         ? -1
                         : QDateTime::currentMSecsSinceEpoch() - waiting->offer.seen_ms);
        record(event);
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
    // Taking a newer offer over a still-waiting guess makes that newer offer
    // the one its eventual prompt must be compared with.
    awaiting_.insert(id, {offer, true, false});
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

} // namespace lapis::desktop
