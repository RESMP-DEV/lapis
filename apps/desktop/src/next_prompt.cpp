#include "next_prompt.hpp"

#include "next_prompt_script.hpp"
#include "platform/posix/private_file.hpp"
#include "platform/updater_process.hpp"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QRandomGenerator>
#include <QTimer>
#include <QUuid>
#include <lapis/session/terminal.hpp>
#include <memory>

#include <algorithm>
#include <cmath>
#include <optional>
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
// The provider chain shares the predict budget: the helper stops asking
// providers before the watchdog kills the process, so a slow provider cannot
// cut the chain off instead of falling through to the next one.
constexpr int kPredictBudgetSeconds = kPredictTimeoutMs / 1000 - 30;
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
// One validated provider entry, or nothing for a malformed one. A rounded
// timeout or token count means the same here as in the helper's own parsing.
std::optional<QJsonObject> clean_provider(const QJsonObject& provider) {
    const auto url = provider.value(QStringLiteral("url")).toString().trimmed();
    const auto model = provider.value(QStringLiteral("model")).toString().trimmed();
    if ((!url.startsWith(QLatin1String("http://")) && !url.startsWith(QLatin1String("https://"))) ||
        url.isEmpty() || model.isEmpty())
        return std::nullopt;
    QJsonObject clean{{QStringLiteral("url"), url.left(300)},
                      {QStringLiteral("model"), model.left(100)},
                      {QStringLiteral("name"),
                       provider.value(QStringLiteral("name")).toString().trimmed().left(40)},
                      {QStringLiteral("timeoutSeconds"), 30},
                      {QStringLiteral("maxTokens"), 4096},
                      {QStringLiteral("temperature"), 0.0}};
    if (clean.value(QStringLiteral("name")).toString().isEmpty())
        clean.insert(QStringLiteral("name"), model.left(40));
    const auto key = provider.value(QStringLiteral("apiKeyEnv")).toString().trimmed();
    if (!key.isEmpty())
        clean.insert(QStringLiteral("apiKeyEnv"), key.left(80));
    if (provider.value(QStringLiteral("allowInsecureHttp")).toBool(false))
        clean.insert(QStringLiteral("allowInsecureHttp"), true);
    if (const auto seconds = provider.value(QStringLiteral("timeoutSeconds")); seconds.isDouble())
        // One attempt must leave room for the next provider: the
        // helper is killed at kPredictTimeoutMs (180s).
        clean.insert(QStringLiteral("timeoutSeconds"),
                     std::clamp(qRound(seconds.toDouble()), 1, 60));
    if (const auto tokens = provider.value(QStringLiteral("maxTokens")); tokens.isDouble())
        clean.insert(QStringLiteral("maxTokens"), std::clamp(qRound(tokens.toDouble()), 128, 8192));
    if (const auto temperature = provider.value(QStringLiteral("temperature"));
        temperature.isDouble())
        clean.insert(QStringLiteral("temperature"), std::clamp(temperature.toDouble(), 0.0, 2.0));
    return clean;
}

void parse_providers(const QJsonObject& object, NextPromptSettings& settings) {
    const auto list = object.value(QStringLiteral("providers"));
    if (!list.isArray())
        return;
    for (const auto& entry : list.toArray()) {
        if (settings.providers.size() >= 8)
            break;
        if (const auto clean = clean_provider(entry.toObject()))
            settings.providers.append(*clean);
    }
}

void parse_context_arms(const QJsonObject& object, NextPromptSettings& settings) {
    const auto arms = object.value(QStringLiteral("contextArms"));
    if (!arms.isArray())
        return;
    // Arms from finished experiments (thread, retrieval, evidence, moves,
    // pack) are dropped, as any unknown name is.
    static const QStringList known{QStringLiteral("base"), QStringLiteral("wide")};
    settings.contextArms.clear();
    for (const auto& arm : arms.toArray())
        if (known.contains(arm.toString()) && !settings.contextArms.contains(arm.toString()))
            settings.contextArms.append(arm.toString());
    if (settings.contextArms.isEmpty())
        settings.contextArms.append(QStringLiteral("base"));
}

void parse_prompt_arms(const QJsonObject& object, NextPromptSettings& settings) {
    const auto arms = object.value(QStringLiteral("promptArms"));
    if (!arms.isArray())
        return;
    static const QStringList known{QStringLiteral("base"), QStringLiteral("checks")};
    settings.promptArms.clear();
    for (const auto& arm : arms.toArray())
        if (known.contains(arm.toString()) && !settings.promptArms.contains(arm.toString()))
            settings.promptArms.append(arm.toString());
    if (settings.promptArms.isEmpty())
        settings.promptArms = QStringList{QStringLiteral("base"), QStringLiteral("checks")};
}

// Folder walls: the same bounds and cleaning as next_prompt.py's wall_list, so
// a zone's index names the same entry on both sides.
constexpr qsizetype kWallLimit = 64;
constexpr qsizetype kWallChars = 1024;
QString unknown_zone() { return QStringLiteral("?"); }

QStringList parse_walls(const QJsonValue& value) {
    QStringList kept;
    if (!value.isArray())
        return kept;
    for (const auto& entry : value.toArray()) {
        if (kept.size() >= kWallLimit)
            break;
        if (!entry.isString())
            continue;
        const auto folder = entry.toString().trimmed();
        if (folder.isEmpty() || folder.size() > kWallChars ||
            !(folder.startsWith(QLatin1Char('/')) || folder == QLatin1String("~") ||
              folder.startsWith(QLatin1String("~/"))))
            continue;
        kept.append(folder);
    }
    return kept;
}

// `path` is `folder` or below it, by whole components.
bool inside(const QString& path, const QString& folder) {
    if (folder == QLatin1String("/"))
        return path.startsWith(QLatin1Char('/'));
    return path == folder || path.startsWith(folder + QLatin1Char('/'));
}

// A folder on this Mac as realpath gives it: `~` expanded, then the longest
// existing prefix's symlinks followed. Empty for a relative path.
QString resolved_here(QString path) {
    if (path == QLatin1String("~"))
        path = QDir::homePath();
    else if (path.startsWith(QLatin1String("~/")))
        path = QDir::homePath() + path.mid(1);
    if (!path.startsWith(QLatin1Char('/')))
        return {};
    path = QDir::cleanPath(path);
    QString head = path;
    QString rest;
    while (!QFileInfo::exists(head)) {
        const auto slash = head.lastIndexOf(QLatin1Char('/'));
        rest.prepend(head.mid(slash));
        head = slash == 0 ? QStringLiteral("/") : head.left(slash);
    }
    const auto canonical = QFileInfo(head).canonicalFilePath();
    return canonical.isEmpty() ? path : QDir::cleanPath(canonical + rest);
}

// Another machine's folder as its launch line has it (see remoteFolder in
// workspace.cpp: `~`, `~/` and a shell word, or a shell word), without the
// shell quoting. Anything but plain and single-quoted text is not read.
std::optional<QString> unquoted(const QString& word) {
    QString text;
    bool quoted = false;
    for (qsizetype at = 0; at < word.size(); ++at) {
        const auto c = word.at(at);
        if (quoted) {
            if (c == QLatin1Char('\''))
                quoted = false;
            else
                text += c;
        } else if (c == QLatin1Char('\'')) {
            quoted = true;
        } else if (c == QLatin1Char('\\') && at + 1 < word.size()) {
            text += word.at(++at);
        } else if (c == QLatin1Char('"') || c == QLatin1Char('$') || c == QLatin1Char('`') ||
                   c.isSpace()) {
            return std::nullopt;
        } else {
            text += c;
        }
    }
    if (quoted)
        return std::nullopt;
    return text;
}

// The zone of `path` among the listed folders, each put in the same form as
// `path` by `form`, which gives nothing when the two cannot be compared.
QString zone_among(const NextPromptSettings& settings, const QString& path,
                   const std::function<std::optional<QString>(const QString&)>& form) {
    bool unknown = false;
    for (qsizetype index = 0; index < settings.off.size(); ++index) {
        const auto folder = form(settings.off.at(index));
        if (!folder)
            unknown = true;
        else if (inside(path, *folder))
            return QStringLiteral("o%1").arg(index);
    }
    // A folder that might be inside an off one is not known to be outside it.
    if (unknown)
        return unknown_zone();
    qsizetype best = -1;
    qsizetype best_length = -1;
    for (qsizetype index = 0; index < settings.separate.size(); ++index) {
        const auto folder = form(settings.separate.at(index));
        if (!folder)
            return unknown_zone();
        if (inside(path, *folder) && folder->size() > best_length) {
            best = index;
            best_length = folder->size();
        }
    }
    return best < 0 ? QString() : QStringLiteral("s%1").arg(best);
}

// A guess may be made for a session in `zone` (see guessable in the helper).
bool guessable(const NextPromptSettings& settings, const QString& zone) {
    if (!settings.walled())
        return true;
    return !zone.startsWith(QLatin1Char('o')) && !(zone == unknown_zone() && !settings.off.isEmpty());
}

// A session in zone `source` may inform a guess for one in `target`.
bool visible(const QString& source, const QString& target) {
    return source == target && target != unknown_zone() && !target.startsWith(QLatin1Char('o'));
}

// The other agents' lines a guess in `zone` may see, without the fields that
// only decide the walls.
QJsonArray walled_agents(const QJsonArray& agents, const NextPromptSettings& settings,
                         const QString& zone) {
    QJsonArray kept;
    for (const auto& value : agents) {
        auto agent = value.toObject();
        const auto machine = agent.take(QStringLiteral("machine")).toString();
        const auto folder = agent.take(QStringLiteral("folder")).toString();
        if (settings.walled() && !visible(folder_zone(settings, machine, folder), zone))
            continue;
        kept.append(agent);
    }
    return kept;
}

// {"separate": [...], "off": [...]} for the helper's --walls.
QString walls_argument(const NextPromptSettings& settings) {
    return QString::fromUtf8(
        QJsonDocument(QJsonObject{{QStringLiteral("separate"),
                                   QJsonArray::fromStringList(settings.separate)},
                                  {QStringLiteral("off"), QJsonArray::fromStringList(settings.off)}})
            .toJson(QJsonDocument::Compact));
}
} // namespace

QString folder_zone(const NextPromptSettings& settings, const QString& machine,
                    const QString& folder) {
    if (!settings.walled())
        return {};
    auto text = folder.trimmed();
    if (machine.isEmpty()) {
        if (text.size() > 1 && text.front() == text.back() &&
            (text.front() == QLatin1Char('\'') || text.front() == QLatin1Char('"')))
            text = text.mid(1, text.size() - 2);
        const auto path = resolved_here(text);
        if (path.isEmpty())
            return unknown_zone();
        return zone_among(settings, path, [](const QString& entry) -> std::optional<QString> {
            return resolved_here(entry);
        });
    }
    // Compared as written: `~` is that machine's home, which is not known here.
    const auto plain = unquoted(text);
    if (!plain || plain->isEmpty())
        return unknown_zone();
    const auto home = plain->startsWith(QLatin1Char('~'));
    if (!home && !plain->startsWith(QLatin1Char('/')))
        return unknown_zone();
    if (home && *plain != QLatin1String("~") && !plain->startsWith(QLatin1String("~/")))
        return unknown_zone();
    const auto path = QDir::cleanPath(*plain);
    // `..` that climbs out of `~` or survives cleaning is not compared as text.
    if (path.startsWith(QLatin1Char('~')) != home ||
        path.split(QLatin1Char('/')).contains(QLatin1String("..")))
        return unknown_zone();
    return zone_among(settings, path, [home](const QString& entry) -> std::optional<QString> {
        if (entry.startsWith(QLatin1Char('~')) != home)
            return std::nullopt;
        return QDir::cleanPath(entry);
    });
}

NextPromptSettings parse_next_prompt(const QJsonValue& value) {
    NextPromptSettings settings;
    const auto object = value.toObject();
    if (object.value(QStringLiteral("auto")).isBool())
        settings.automatic = object.value(QStringLiteral("auto")).toBool();
    if (const auto model = object.value(QStringLiteral("model")).toString().trimmed();
        !model.isEmpty())
        settings.model = model.left(100);
    settings.effort = object.value(QStringLiteral("effort")).toString().trimmed().left(20);
    if (const auto cap = object.value(QStringLiteral("maxPerHour")); cap.isDouble())
        settings.maxPerHour = std::max(0, cap.toInt());
    parse_providers(object, settings);
    parse_context_arms(object, settings);
    parse_prompt_arms(object, settings);
    if (const auto minutes = object.value(QStringLiteral("returnMinutes")); minutes.isDouble())
        settings.returnMinutes = std::max(1, minutes.toInt());
    settings.separate = parse_walls(object.value(QStringLiteral("separate")));
    settings.off = parse_walls(object.value(QStringLiteral("off")));
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
    QString error;
    if (!session::posix::write_private_file(script_path_, QByteArray(kNextPromptScript), &error))
        qWarning() << "Next prompt: cannot write the helper:" << error;
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
    if (deferred_.remove(id)) {
        run(id, true);
        return;
    }
    returned(id);
}

void NextPrompt::returned(const QString& id) {
    const auto offer = offers_.constFind(id);
    if (!settings_.automatic || offer == offers_.cend() || offer->made_ms == 0 ||
        returns_decided_.contains(offer->key) || awaiting_.value(id).filled)
        return;
    const auto minutes = (QDateTime::currentMSecsSinceEpoch() - offer->made_ms) / 60000;
    if (minutes < settings_.returnMinutes)
        return;
    returns_decided_.insert(offer->key);
    auto event = about(*offer, id);
    event.insert(QStringLiteral("event"), QStringLiteral("return"));
    event.insert(QStringLiteral("age_minutes"), static_cast<int>(minutes));
    record(event);
    run(id, true, static_cast<int>(minutes));
}

double NextPrompt::confidence(const QString& id) const { return offers_.value(id).p; }

void NextPrompt::run(const QString& id, bool force, int returned) {
    if (!settings_.automatic)
        return;
    if (const auto shown = offers_.constFind(id); shown != offers_.cend())
        previous_.insert(id, {offerConversation(*shown), shown->turn, shown->seen_ms != 0});
    deferred_.remove(id);
    owed_.remove(id);
    dropRestored(id, "new_turn");
    withdraw(id, returned >= 0 ? Withdrawal::returned : Withdrawal::next_turn);
    const auto agent = lookup_(id);
    if (!agent || (agent->cli != QLatin1String("claude") && agent->cli != QLatin1String("codex") &&
                   agent->cli != QLatin1String("omp")))
        return;
    if (auto old = running_.take(id); old.process)
        old.process->stopGroup();
    // An agent known to be in an off folder is not read, let alone sent to a
    // model. One whose folder cannot be told from here is read where it runs,
    // and predict() decides from what its conversation records.
    if (folder_zone(settings_, agent->machine, agent->folder).startsWith(QLatin1Char('o'))) {
        skippedOff(id);
        return;
    }
    if (!budgetAvailable(id))
        return;
    const auto generation = ++generation_;
    running_.insert(id, {generation, *agent, {}});
    emit stateChanged();
    runContext(id, generation, *agent, Stage::context,
               [this, id, generation, force, returned](const QJsonObject& context) {
                   settle(id, context);
                   // A show can arrive while this fetch is in flight; consume it
                   // here so the repeat is guessed rather than deferred again.
                   const bool shown_during_run = pending_show_.remove(id);
                   // A repeat: the person has sent nothing since the last guess
                   // and never saw it. Guess again when they show this agent.
                   const auto last = previous_.value(id);
                   const auto conversation = contextConversation(context);
                   if (!force && !last.seen && last.turn >= 0 &&
                       last.conversation == conversation &&
                       last.turn == context.value(QStringLiteral("turn")).toInt()) {
                       if (shown_during_run) {
                           predict(id, generation, context, returned);
                           return;
                       }
                       running_.remove(id);
                       deferred_.insert(id);
                       record({{QStringLiteral("event"), QStringLiteral("skipped")},
                               {QStringLiteral("agent"), id},
                               {QStringLiteral("reason"), QStringLiteral("unseen_repeat")}});
                       return;
                   }
                   predict(id, generation, context, returned);
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
    // Resolved where the helper runs; without walls the words are as before.
    if (settings_.walled())
        words << QStringLiteral("--walls") << walls_argument(settings_);
    return words;
}

void NextPrompt::skippedOff(const QString& id) {
    running_.remove(id);
    emit stateChanged();
    record({{QStringLiteral("event"), QStringLiteral("skipped")},
            {QStringLiteral("agent"), id},
            {QStringLiteral("reason"), QStringLiteral("off_folder")}});
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
                failed(id, agent, stage, why,
                       document.object().value(QStringLiteral("kind")).toString());
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

// The helper's predict arguments. --providers is passed either way: `[]`
// keeps the CLI route rather than letting the helper read lapis.json itself.
struct PredictArguments {
    QString script;
    QString model;
    QString claude;
    QString effort;
    QString context_arm;
    QString prompt_arm;
    QJsonArray providers;
};

QStringList predict_arguments(const PredictArguments& args) {
    QStringList arguments{args.script, QStringLiteral("predict"),  QStringLiteral("--model"),
                          args.model,  QStringLiteral("--claude"), args.claude};
    if (!args.effort.isEmpty())
        arguments << QStringLiteral("--effort") << args.effort;
    arguments << QStringLiteral("--context-arm") << args.context_arm
              << QStringLiteral("--prompt-arm") << args.prompt_arm << QStringLiteral("--providers")
              << QString::fromUtf8(QJsonDocument(args.providers).toJson(QJsonDocument::Compact))
              << QStringLiteral("--budget-seconds") << QString::number(kPredictBudgetSeconds);
    return arguments;
}

void NextPrompt::predict(const QString& id, quint64 generation, const QJsonObject& context,
                         int returned) {
    // The guess's zone, read where the agent runs from the folders its
    // conversation records (the helper's `wall`), checked against where lapis
    // started it when that can be told from here. A disagreement, or no
    // answer from the helper, is unknown: the guess sees no other session.
    QString zone;
    if (settings_.walled()) {
        const auto& target = running_.value(id).agent;
        const auto here = folder_zone(settings_, target.machine, target.folder);
        const auto wall = context.value(QStringLiteral("wall"));
        zone = wall.isString() ? wall.toString() : unknown_zone();
        if (here != unknown_zone() && here != zone)
            zone = unknown_zone();
        if (!guessable(settings_, zone) || here.startsWith(QLatin1Char('o'))) {
            skippedOff(id);
            return;
        }
    }
    const auto claude = program_(QStringLiteral("claude"));
    // The chain replaces the CLI call, so the CLI is only required when it
    // is the route that answers.
    if (claude.isEmpty() && settings_.providers.isEmpty()) {
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
        return;
    }
    started_.push_back({clock_.elapsed(), generation});
    const auto agent = running_.value(id).agent;
    QJsonObject about{{QStringLiteral("title"), agent.title},
                      {QStringLiteral("folder"), agent.folder},
                      {QStringLiteral("cli"), agent.cli},
                      {QStringLiteral("machine"), agent.machine},
                      {QStringLiteral("category"), agent.category}};
    QJsonObject bundle{
        {QStringLiteral("agent"), about},
        {QStringLiteral("screen"), agent.screen},
        {QStringLiteral("context"), context},
        {QStringLiteral("agents"), walled_agents(agents_(), settings_, zone)},
        {QStringLiteral("taken"), QJsonArray::fromStringList(taken_)},
        {QStringLiteral("target"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {QStringLiteral("time"),
         QDateTime::currentDateTime().toString(QStringLiteral("ddd h:mm ap"))}};
    if (returned >= 0)
        bundle.insert(QStringLiteral("returned_minutes"), returned);
    // The two live comparisons, each drawn on its own for every guess.
    Arms arms_made;
    arms_made.returned = returned;
    arms_made.model = settings_.model;
    auto* random = QRandomGenerator::global();
    QStringList context_arms = settings_.contextArms;
    if (context_arms.isEmpty())
        context_arms = QStringList{QStringLiteral("base")};
    arms_made.context = context_arms.at(random->bounded(context_arms.size()));
    arms_made.prompt = settings_.promptArms.isEmpty()
                           ? QStringLiteral("base")
                           : settings_.promptArms.at(random->bounded(settings_.promptArms.size()));
    const auto arguments = predict_arguments({.script = script_path_,
                                              .model = settings_.model,
                                              .claude = claude,
                                              .effort = settings_.effort,
                                              .context_arm = arms_made.context,
                                              .prompt_arm = arms_made.prompt,
                                              .providers = settings_.providers});
    start(id, generation, program_(QStringLiteral("python3")), arguments, line(bundle),
          Stage::predict, [this, id, agent, context, arms_made](const QJsonObject& answer) {
              running_.remove(id);
              offer(id, agent, context, answer, arms_made);
              emit stateChanged();
          });
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void NextPrompt::offer(const QString& id, const Agent& agent, const QJsonObject& context,
                       // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
                       const QJsonObject& answer, const Arms& arms) {
    const auto candidates = answer.value(QStringLiteral("candidates")).toArray();
    const auto top = candidates.isEmpty() ? QJsonObject() : candidates.first().toObject();
    const auto text = top.value(QStringLiteral("text")).toString();
    const auto probability = top.value(QStringLiteral("p"));
    // The helper always emits a numeric p (0.0 for unscored candidates, flagged
    // `scored: false`), so isDouble() only rejects replies that skipped that
    // normalization. A reported 0.0 is still offered; whether the model scored
    // the top guess is recorded as top_scored below.
    const bool shown = !text.isEmpty() && probability.isDouble() && settings_.automatic;
    const Offer made{QStringLiteral("%1:%2.%3").arg(id, run_).arg(++offers_made_),
                     text,
                     context.value(QStringLiteral("conversation")).toString(),
                     contextConversation(context),
                     context.value(QStringLiteral("turn")).toInt(),
                     0,
                     last_reply(context),
                     probability.toDouble(),
                     QDateTime::currentMSecsSinceEpoch()};
    succeeded();
    auto event = about(made, id);
    event.insert(QStringLiteral("event"), QStringLiteral("predicted"));
    event.insert(QStringLiteral("machine"), agent.machine);
    event.insert(QStringLiteral("cli"), agent.cli);
    const auto answer_model = answer.value(QStringLiteral("model")).toString().trimmed();
    event.insert(QStringLiteral("model"),
                 answer_model.isEmpty() ? arms.model : answer_model.left(100));
    if (const auto provider = answer.value(QStringLiteral("provider")).toString().trimmed();
        !provider.isEmpty())
        event.insert(QStringLiteral("provider"), provider.left(40));
    event.insert(QStringLiteral("context_arm"), arms.context);
    event.insert(QStringLiteral("prompt_arm"), arms.prompt);
    if (arms.returned >= 0)
        event.insert(QStringLiteral("returned_minutes"), arms.returned);
    event.insert(QStringLiteral("category"), answer.value(QStringLiteral("category")));
    event.insert(QStringLiteral("candidates"), candidates);
    event.insert(QStringLiteral("top_scored"), top.value(QStringLiteral("scored")).toBool());
    event.insert(QStringLiteral("shown"), shown);
    event.insert(QStringLiteral("ms"), answer.value(QStringLiteral("ms")));
    record(event);
    if (!shown)
        return;
    previous_.insert(id, {offerConversation(made), made.turn, made.seen_ms != 0});
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
void NextPrompt::failed(const QString& id, const Agent& agent, Stage stage, const QString& why,
                        const QString& kind) {
    const QStringList known{
        QStringLiteral("no transcript"),           QStringLiteral("no Claude Code CLI on this Mac"),
        QStringLiteral("invalid conversation id"), QStringLiteral("timeout"),
        QStringLiteral("output too large"),        QStringLiteral("invalid helper JSON"),
        QStringLiteral("helper unavailable")};
    const bool from_helper = !known.contains(why);
    const auto reason = from_helper ? QStringLiteral("helper failed") : why;
    QJsonObject event{{QStringLiteral("event"), QStringLiteral("failed")},
                      {QStringLiteral("agent"), id},
                      {QStringLiteral("machine"), agent.machine},
                      {QStringLiteral("cli"), agent.cli},
                      {QStringLiteral("stage"), stage == Stage::predict
                                                    ? QStringLiteral("predict")
                                                    : QStringLiteral("context")},
                      {QStringLiteral("error"), reason}};
    // A message that came with a kind was cleaned to one line by failure() in
    // next_prompt.py; any other helper text stays out of the log.
    if (from_helper && !why.isEmpty() && !kind.isEmpty())
        event.insert(QStringLiteral("detail"), why.left(160));
    if (!kind.isEmpty())
        event.insert(QStringLiteral("kind"), kind.left(16));
    record(event);
    if (stage == Stage::predict || kind == QStringLiteral("timeout")) {
        ++failure_streak_;
        static const QHash<QString, QString> plain{
            {QStringLiteral("limit"), QStringLiteral("a model usage limit")},
            {QStringLiteral("auth"), QStringLiteral("a model sign-in")},
            {QStringLiteral("timeout"), QStringLiteral("timeouts")},
            {QStringLiteral("network"), QStringLiteral("the network")},
            {QStringLiteral("malformed"), QStringLiteral("unreadable answers")}};
        last_failure_ = plain.value(kind, from_helper ? why.left(100) : reason);
        emit failingChanged();
    }
}

void NextPrompt::succeeded() {
    if (failure_streak_ == 0)
        return;
    failure_streak_ = 0;
    last_failure_.clear();
    emit failingChanged();
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

QString NextPrompt::contextConversation(const QJsonObject& context) {
    const auto id = context.value(QStringLiteral("conversation_id")).toString();
    return id.isEmpty() ? context.value(QStringLiteral("conversation")).toString() : id;
}

QString NextPrompt::offerConversation(const Offer& offer) {
    return offer.conversation_id.isEmpty() ? offer.conversation : offer.conversation_id;
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
    recordWithdrawn(offer, id,
                    why == Withdrawal::off        ? "off"
                    : why == Withdrawal::returned ? "return"
                                                  : "new_turn");
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
    const auto conversation = contextConversation(context);
    const auto offer_conversation = offerConversation(waiting->offer);
    if (!offer_conversation.isEmpty() && conversation != offer_conversation) {
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
    if (exact && waiting->filled) {
        taken_.append(sent.trimmed().left(500));
        while (taken_.size() > 40)
            taken_.removeFirst();
    }
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
        // These are labels (what the person did with each guess or move), the
        // one thing here that cannot be regenerated: the previous file is
        // archived under its date, never deleted.
        const auto previous = log_path_ + QStringLiteral(".1");
        if (QFile::exists(previous) &&
            !QFile::rename(previous, log_path_ + QDateTime::currentDateTimeUtc().toString(
                                                     QStringLiteral("'.'yyyyMMdd-HHmmss")))) {
            qWarning() << "Next prompt: cannot archive the previous log";
            return;
        }
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
    return cli == QLatin1String("claude") || cli == QLatin1String("codex") ||
           cli == QLatin1String("omp");
}
} // namespace

bool NextPrompt::guesses(const QString& cli) const { return chats(cli); }

QJsonObject NextPrompt::encodeOffer(const Offer& offer, qsizetype& budget) const {
    if (offer.key.size() > 400 || offer.conversation.size() > 200 ||
        offer.conversation_id.size() > 200 || offer.text.isEmpty() ||
        offer.text.size() > kSavedText || offer.text.size() + offer.said.size() > budget)
        return {};
    budget -= offer.text.size() + offer.said.size();
    return {{QStringLiteral("key"), offer.key},
            {QStringLiteral("text"), offer.text},
            {QStringLiteral("conversation"), offer.conversation},
            {QStringLiteral("conversationId"), offer.conversation_id},
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
    offer.conversation_id = object.value(QStringLiteral("conversationId")).toString();
    offer.turn = object.value(QStringLiteral("turn")).toInt(-1);
    offer.seen_ms = static_cast<qint64>(object.value(QStringLiteral("seenMs")).toDouble(-1));
    offer.said = object.value(QStringLiteral("said")).toString().left(said_limit);
    if (id.isEmpty() || id.size() > 200 || !offer.key.startsWith(id + QLatin1Char(':')) ||
        offer.key.size() > 400 || offer.text.isEmpty() || offer.text.size() > kSavedText ||
        offer.conversation.size() > 200 || offer.conversation_id.size() > 200 || offer.turn < 0 ||
        offer.seen_ms < 0)
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
            !restorable(entry.key(), offerConversation(*offer))) {
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
            else if (!known->conversation.isEmpty() && !offerConversation(*offer).isEmpty() &&
                     known->conversation != offerConversation(*offer))
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
    const auto conversation = contextConversation(context);
    if (conversation != offerConversation(offer)) {
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
