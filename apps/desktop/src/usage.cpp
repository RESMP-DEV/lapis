#include "usage.hpp"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QThread>
#include <QTimeZone>
#include <algorithm>
#include <array>
#include <utility>

namespace lapis::desktop {
namespace {
constexpr qint64 kChunk = qint64{8} << 20;
constexpr qsizetype kLongestAnswer = qsizetype{4} << 20;
constexpr int kChartDays = 30;

// The text of the JSON string after `key` (which ends with its opening
// quote), searching from `from`; empty when absent.
QByteArrayView string_after(QByteArrayView line, QByteArrayView key, qsizetype from = 0) {
    const auto at = line.indexOf(key, from);
    if (at < 0)
        return {};
    const auto start = at + key.size();
    for (auto i = start; i < line.size(); ++i) {
        if (line[i] == '\\')
            ++i;
        else if (line[i] == '"')
            return line.sliced(start, i - start);
    }
    return {};
}

// The JSON object that opens at `at`, through its closing brace.
QJsonObject object_at(QByteArrayView line, qsizetype at) {
    if (at < 0 || at >= line.size() || line[at] != '{')
        return {};
    int depth = 0;
    bool quoted = false;
    for (auto i = at; i < line.size(); ++i) {
        const char c = line[i];
        if (quoted) {
            if (c == '\\')
                ++i;
            else if (c == '"')
                quoted = false;
        } else if (c == '"') {
            quoted = true;
        } else if (c == '{') {
            ++depth;
        } else if (c == '}' && --depth == 0) {
            return QJsonDocument::fromJson(line.sliced(at, i - at + 1).toByteArray()).object();
        }
    }
    return {};
}

qint64 number(const QJsonObject& object, const char* key) {
    return static_cast<qint64>(object.value(QLatin1String(key)).toDouble());
}

// Codex counts cached and cache-written input inside input_tokens.
TokenCount codex_count(const QJsonObject& usage) {
    const auto cached = number(usage, "cached_input_tokens");
    const auto written = number(usage, "cache_write_input_tokens");
    return {.input = std::max<qint64>(0, number(usage, "input_tokens") - cached - written),
            .cacheRead = cached,
            .cacheWrite = written,
            .output = number(usage, "output_tokens")};
}

bool covers(const TokenCount& later, const TokenCount& earlier) {
    return later.input >= earlier.input && later.cacheRead >= earlier.cacheRead &&
           later.cacheWrite >= earlier.cacheWrite && later.output >= earlier.output;
}

QString window_label(int minutes) {
    if (minutes == 7 * 24 * 60)
        return QStringLiteral("Week");
    if (minutes == 24 * 60)
        return QStringLiteral("Day");
    if (minutes > 0 && minutes % 60 == 0)
        return minutes == 60 ? QStringLiteral("Hour")
                             : QStringLiteral("%1 hours").arg(minutes / 60);
    return minutes > 0 ? QStringLiteral("%1 minutes").arg(minutes) : QStringLiteral("Limit");
}

QVariantMap token_map(const TokenCount& count) {
    return {{QStringLiteral("total"), count.total()},
            {QStringLiteral("input"), count.input},
            {QStringLiteral("cacheRead"), count.cacheRead},
            {QStringLiteral("cacheWrite"), count.cacheWrite},
            {QStringLiteral("output"), count.output}};
}

// Deleted once it has exited, so it is reaped rather than left running.
void stop(QProcess* process) {
    if (process->state() == QProcess::NotRunning) {
        process->deleteLater();
        return;
    }
    QObject::connect(process, &QProcess::finished, process, &QObject::deleteLater);
    process->kill();
}

const std::array<std::pair<const char*, const char*>, 2> providers_shown{
    {{"codex", "Codex"}, {"claude", "Claude"}}};
} // namespace

TokenCount& TokenCount::operator+=(const TokenCount& other) {
    input += other.input;
    cacheRead += other.cacheRead;
    cacheWrite += other.cacheWrite;
    output += other.output;
    return *this;
}
TokenCount& TokenCount::operator-=(const TokenCount& other) {
    input -= other.input;
    cacheRead -= other.cacheRead;
    cacheWrite -= other.cacheWrite;
    output -= other.output;
    return *this;
}

void TokenLedger::scan(QDate since) {
    since_ = since;
    walk(Source::codex, roots_.codex, codex_);
    walk(Source::claude, roots_.claude, claude_);
}

void TokenLedger::walk(Source source, const QString& root, QHash<QString, File>& files) {
    if (root.isEmpty())
        return;
    const QDateTime start(since_, QTime(0, 0));
    QDirIterator it(
        root,
        {source == Source::codex ? QStringLiteral("rollout-*.jsonl") : QStringLiteral("*.jsonl")},
        QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        const auto info = it.fileInfo();
        const auto modified = info.lastModified();
        const auto found = files.find(info.filePath());
        if (found == files.end() && modified < start)
            continue;
        auto& file = found == files.end() ? files[info.filePath()] : *found;
        if (file.modified == modified && file.size == info.size())
            continue;
        // Rewritten rather than appended: read it again from the top.
        // Claude messages already counted elsewhere stay counted once.
        if (info.size() < file.offset)
            file = File{};
        file.modified = modified;
        file.size = info.size();
        read(source, info.filePath(), file);
    }
}

void TokenLedger::read(Source source, const QString& path, File& file) {
    QFile input(path);
    if (!input.open(QIODevice::ReadOnly) || !input.seek(file.offset))
        return;
    const bool codex = source == Source::codex;
    QByteArray carry;
    for (;;) {
        auto chunk = input.read(kChunk);
        if (chunk.isEmpty())
            break;
        bytes_read_ += chunk.size();
        const QByteArray data = carry.isEmpty() ? std::move(chunk) : carry + chunk;
        qsizetype start = 0;
        for (auto end = data.indexOf('\n'); end >= 0; end = data.indexOf('\n', start)) {
            const QByteArrayView line(data.constData() + start, end - start);
            if (codex)
                codexLine(line, file);
            else
                claudeLine(line, file);
            file.offset += end - start + 1;
            start = end + 1;
        }
        // A line still being written is read whole next time.
        carry = data.sliced(start);
    }
}

// "2026-09-24T23:32:08.070Z" to the local day, cached per quarter hour.
QDate TokenLedger::localDay(QByteArrayView stamp) {
    if (stamp.size() < 20 || stamp[4] != '-' || stamp[10] != 'T' || stamp[13] != ':')
        return {};
    const auto field = [&](qsizetype at, qsizetype size) {
        int value = 0;
        for (auto i = at; i < at + size; ++i) {
            if (stamp[i] < '0' || stamp[i] > '9')
                return -1;
            value = value * 10 + (stamp[i] - '0');
        }
        return value;
    };
    const QDate date(field(0, 4), field(5, 2), field(8, 2));
    const int hour = field(11, 2);
    const int minute = field(14, 2);
    if (!date.isValid() || hour < 0 || hour > 23 || minute < 0 || minute > 59)
        return {};
    if (!stamp.endsWith('Z'))
        return QDateTime::fromString(QString::fromLatin1(stamp), Qt::ISODateWithMs)
            .toLocalTime()
            .date();
    const qint64 quarter = (date.toJulianDay() * 24 + hour) * 4 + minute / 15;
    const auto cached = hours_.constFind(quarter);
    if (cached != hours_.cend())
        return *cached;
    const auto day =
        QDateTime(date, QTime(hour, minute / 15 * 15), QTimeZone::UTC).toLocalTime().date();
    hours_.insert(quarter, day);
    return day;
}

// Codex writes the session's running total after each model call, and
// sometimes repeats the latest report unchanged; the difference from the
// previous total is what was used since. A file's first report, and a total
// that went down, count only that call: a fork can start from its parent's
// total. Only the line's head is searched for its kind, since tool output can
// be long.
void TokenLedger::codexLine(QByteArrayView line, File& file) {
    const auto head = line.first(std::min<qsizetype>(line.size(), 200));
    if (head.contains("\"type\":\"turn_context\"")) {
        if (const auto model = string_after(line, "\"model\":\""); !model.isEmpty())
            file.model = QString::fromUtf8(model);
        return;
    }
    if (!head.contains("\"payload\":{\"type\":\"token_count\""))
        return;
    const auto at = line.indexOf("\"total_token_usage\":");
    if (at < 0)
        return;
    const auto total = codex_count(object_at(line, at + 20));
    TokenCount used = total;
    if (file.started && covers(total, file.total)) {
        used -= file.total;
    } else {
        const auto last = line.indexOf("\"last_token_usage\":");
        used = last < 0 ? TokenCount{} : codex_count(object_at(line, last + 19));
    }
    file.total = total;
    file.started = true;
    if (used.total() == 0)
        return;
    const auto day = localDay(string_after(line, "\"timestamp\":\""));
    if (!day.isValid() || day < since_)
        return;
    file.days[day][file.model.isEmpty() ? QStringLiteral("codex") : file.model] += used;
}

// Claude writes each streamed block of a message as its own line carrying the
// message's usage so far (earlier lines can have a partial output count), and
// a resumed session repeats earlier messages. Each message and request counts
// once across all files, at the largest usage any of its lines reported.
void TokenLedger::claudeLine(QByteArrayView line, File& file) {
    const auto message = line.indexOf("\"message\":{");
    if (message < 0)
        return;
    const auto head = line.sliced(message, std::min<qsizetype>(line.size() - message, 400));
    if (!head.contains("\"role\":\"assistant\""))
        return;
    const auto usage = line.lastIndexOf("\"usage\":{");
    if (usage < 0)
        return;
    const auto model = string_after(line, "\"model\":\"", message);
    if (model == "<synthetic>")
        return;
    const auto day = localDay(string_after(line, "\"timestamp\":\"", usage));
    if (!day.isValid() || day < since_)
        return;
    const auto object = object_at(line, usage + 8);
    TokenCount used{.input = number(object, "input_tokens"),
                    .cacheRead = number(object, "cache_read_input_tokens"),
                    .cacheWrite = number(object, "cache_creation_input_tokens"),
                    .output = number(object, "output_tokens")};
    if (const auto id = string_after(line, "\"id\":\"", message); !id.isEmpty()) {
        const auto request = string_after(line, "\"requestId\":\"");
        const quint64 key = qHash(id, 0x5bd1e995U) ^ (qHash(request, 0x27d4eb2dU) * 31U);
        auto& counted = seen_[key];
        const TokenCount largest{.input = std::max(counted.input, used.input),
                                 .cacheRead = std::max(counted.cacheRead, used.cacheRead),
                                 .cacheWrite = std::max(counted.cacheWrite, used.cacheWrite),
                                 .output = std::max(counted.output, used.output)};
        used = largest;
        used -= counted;
        counted = largest;
    }
    if (used.total() > 0)
        file.days[day][model.isEmpty() ? QStringLiteral("claude") : QString::fromUtf8(model)] +=
            used;
}

QHash<QString, TokenLedger::Days> TokenLedger::days() const {
    QHash<QString, Days> result;
    const auto gather = [&](const QString& provider, const QHash<QString, File>& files) {
        auto& days = result[provider];
        for (const auto& file : files)
            for (auto day = file.days.cbegin(); day != file.days.cend(); ++day)
                for (auto model = day->cbegin(); model != day->cend(); ++model)
                    days[day.key()][model.key()] += *model;
    };
    gather(QStringLiteral("codex"), codex_);
    gather(QStringLiteral("claude"), claude_);
    return result;
}

PlanLimits codex_limits(const QJsonObject& result) {
    PlanLimits limits;
    const auto main = result.value(QStringLiteral("rateLimits")).toObject();
    limits.plan = main.value(QStringLiteral("planType")).toString();
    const auto add = [&](const QJsonObject& bucket, const QString& name) {
        for (const auto* which : {"primary", "secondary"}) {
            const auto window = bucket.value(QLatin1String(which)).toObject();
            if (window.isEmpty())
                continue;
            const int minutes = window.value(QStringLiteral("windowDurationMins")).toInt();
            const auto resets =
                static_cast<qint64>(window.value(QStringLiteral("resetsAt")).toDouble());
            auto label = window_label(minutes);
            if (!name.isEmpty())
                label += QStringLiteral(", ") + name;
            limits.windows.push_back(
                {.label = label,
                 .percent = window.value(QStringLiteral("usedPercent")).toDouble(),
                 .resets = resets > 0 ? QDateTime::fromSecsSinceEpoch(resets) : QDateTime(),
                 .minutes = minutes});
        }
    };
    add(main, QString());
    // Other buckets (a model with its own limit), each named by the server.
    const auto others = result.value(QStringLiteral("rateLimitsByLimitId")).toObject();
    const auto main_id = main.value(QStringLiteral("limitId")).toString();
    for (auto it = others.begin(); it != others.end(); ++it) {
        if (it.key() == main_id)
            continue;
        const auto bucket = it.value().toObject();
        const auto name = bucket.value(QStringLiteral("limitName")).toString();
        add(bucket, name.isEmpty() ? it.key() : name);
    }
    if (!main.value(QStringLiteral("rateLimitReachedType")).toString().isEmpty())
        limits.note = QStringLiteral("Limit reached");
    limits.resetCredits = result.value(QStringLiteral("rateLimitResetCredits"))
                              .toObject()
                              .value(QStringLiteral("availableCount"))
                              .toInt();
    return limits;
}

PlanLimits claude_limits(const QJsonObject& response) {
    PlanLimits limits;
    limits.plan = response.value(QStringLiteral("subscription_type")).toString();
    if (!response.value(QStringLiteral("rate_limits_available")).toBool()) {
        limits.note = QStringLiteral("No plan limits for this login");
        return limits;
    }
    const auto rates = response.value(QStringLiteral("rate_limits")).toObject();
    const auto add = [&](const QJsonObject& window, const QString& label, int minutes) {
        if (window.isEmpty() || window.value(QStringLiteral("utilization")).isNull())
            return;
        limits.windows.push_back(
            {.label = label,
             .percent = window.value(QStringLiteral("utilization")).toDouble(),
             .resets = QDateTime::fromString(window.value(QStringLiteral("resets_at")).toString(),
                                             Qt::ISODateWithMs),
             .minutes = minutes});
    };
    add(rates.value(QStringLiteral("five_hour")).toObject(), QStringLiteral("5 hours"), 5 * 60);
    add(rates.value(QStringLiteral("seven_day")).toObject(), QStringLiteral("Week"), 7 * 24 * 60);
    // Per-model weekly windows, as the server names them.
    const auto scoped = rates.value(QStringLiteral("model_scoped")).toArray();
    for (const auto& item : scoped) {
        const auto window = item.toObject();
        add(window,
            QStringLiteral("Week, ") + window.value(QStringLiteral("display_name")).toString(),
            7 * 24 * 60);
    }
    if (scoped.isEmpty()) {
        add(rates.value(QStringLiteral("seven_day_opus")).toObject(), QStringLiteral("Week, Opus"),
            7 * 24 * 60);
        add(rates.value(QStringLiteral("seven_day_sonnet")).toObject(),
            QStringLiteral("Week, Sonnet"), 7 * 24 * 60);
    }
    return limits;
}

Usage::Usage(Programs programs, TokenLedger::Roots roots, QObject* parent)
    : QObject(parent), programs_(std::move(programs)), ledger_(std::move(roots)) {
    pool_.setMaxThreadCount(1);
    connect(&limits_timer_, &QTimer::timeout, this, [this] {
        limits_timer_.setInterval(kLimitsMs);
        askLimits();
    });
    tokens_timer_.setSingleShot(true);
    connect(&tokens_timer_, &QTimer::timeout, this, &Usage::count);
}

Usage::~Usage() {
    setActive(false);
    for (auto* process : findChildren<QProcess*>(Qt::FindDirectChildrenOnly)) {
        process->disconnect(this);
        process->kill();
        process->waitForFinished(1000);
    }
    pool_.waitForDone();
}

void Usage::setActive(bool active) {
    if (active == active_)
        return;
    active_ = active;
    if (active_) {
        // After the window and its agents have started.
        limits_timer_.start(kFirstLimitsMs);
        tokens_timer_.start(kFirstCountMs);
        return;
    }
    limits_timer_.stop();
    tokens_timer_.stop();
    while (!queries_.empty())
        finish(queries_.front().provider, std::nullopt, QString());
}

void Usage::refresh() {
    askLimits();
    count();
}

QDate Usage::today() const { return today_.isValid() ? today_ : QDate::currentDate(); }

void Usage::askLimits() {
    for (const auto& [id, name] : providers_shown)
        ask(QString::fromLatin1(id));
}

void Usage::ask(const QString& provider) {
    if (std::any_of(queries_.begin(), queries_.end(),
                    [&](const Query& query) { return query.provider == provider; }))
        return;
    const auto program = programs_ ? programs_(provider) : QString();
    if (program.isEmpty()) {
        limits_.remove(provider);
        emit changed();
        return;
    }
    auto* process = new QProcess(this);
    process->setProgram(program);
    process->setWorkingDirectory(QDir::tempPath());
    process->setStandardErrorFile(QProcess::nullDevice());
    if (provider == QLatin1String("codex")) {
        process->setArguments({QStringLiteral("app-server")});
    } else {
        // No settings (so none of the person's hooks run) and no transcript.
        process->setArguments({QStringLiteral("-p"), QStringLiteral("--input-format"),
                               QStringLiteral("stream-json"), QStringLiteral("--output-format"),
                               QStringLiteral("stream-json"), QStringLiteral("--verbose"),
                               QStringLiteral("--setting-sources"), QString(),
                               QStringLiteral("--no-session-persistence")});
    }
    queries_.push_back({provider, process, {}});
    connect(process, &QProcess::readyReadStandardOutput, this,
            [this, provider] { answer(provider); });
    connect(process, &QProcess::finished, this, [this, provider] {
        finish(provider, std::nullopt, QStringLiteral("ended without an answer"));
    });
    connect(process, &QProcess::errorOccurred, this,
            [this, provider](QProcess::ProcessError error) {
                if (error == QProcess::FailedToStart)
                    finish(provider, std::nullopt, QStringLiteral("could not start"));
            });
    QTimer::singleShot(kAnswerMs, process, [this, provider] {
        finish(provider, std::nullopt, QStringLiteral("did not answer"));
    });
    process->start();
    const QJsonObject request =
        provider == QLatin1String("codex")
            ? QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                          {QStringLiteral("id"), 1},
                          {QStringLiteral("method"), QStringLiteral("initialize")},
                          {QStringLiteral("params"),
                           QJsonObject{
                               {QStringLiteral("clientInfo"),
                                QJsonObject{{QStringLiteral("name"), QStringLiteral("lapis")},
                                            {QStringLiteral("version"), QStringLiteral("1")}}}}}}
            : QJsonObject{{QStringLiteral("type"), QStringLiteral("control_request")},
                          {QStringLiteral("request_id"), QStringLiteral("usage")},
                          {QStringLiteral("request"),
                           QJsonObject{{QStringLiteral("subtype"), QStringLiteral("get_usage")},
                                       {QStringLiteral("skip_behaviors"), true}}}};
    process->write(QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n');
}

void Usage::answer(const QString& provider) {
    const auto found = std::find_if(queries_.begin(), queries_.end(),
                                    [&](const Query& query) { return query.provider == provider; });
    if (found == queries_.end() || !found->process)
        return;
    auto* process = found->process.data();
    found->pending += process->readAllStandardOutput();
    if (found->pending.size() > kLongestAnswer)
        return finish(provider, std::nullopt, QStringLiteral("answered too much"));
    const bool codex = provider == QLatin1String("codex");
    for (auto end = found->pending.indexOf('\n'); end >= 0; end = found->pending.indexOf('\n')) {
        const auto message = QJsonDocument::fromJson(found->pending.left(end)).object();
        found->pending.remove(0, end + 1);
        if (codex) {
            const auto id = message.value(QStringLiteral("id")).toInt();
            if (id == 1) {
                process->write(R"({"jsonrpc":"2.0","method":"initialized"})"
                               "\n"
                               R"({"jsonrpc":"2.0","id":2,"method":"account/rateLimits/read"})"
                               "\n");
            } else if (id == 2) {
                if (message.contains(QStringLiteral("result")))
                    return finish(provider,
                                  codex_limits(message.value(QStringLiteral("result")).toObject()),
                                  QString());
                return finish(provider, std::nullopt,
                              message.value(QStringLiteral("error"))
                                  .toObject()
                                  .value(QStringLiteral("message"))
                                  .toString(QStringLiteral("refused")));
            }
            continue;
        }
        if (message.value(QStringLiteral("type")).toString() != QLatin1String("control_response"))
            continue;
        const auto response = message.value(QStringLiteral("response")).toObject();
        if (response.value(QStringLiteral("request_id")).toString() != QLatin1String("usage"))
            continue;
        if (response.value(QStringLiteral("subtype")).toString() == QLatin1String("success"))
            return finish(provider,
                          claude_limits(response.value(QStringLiteral("response")).toObject()),
                          QString());
        return finish(provider, std::nullopt,
                      response.value(QStringLiteral("error")).toString(QStringLiteral("refused")));
    }
}

void Usage::finish(const QString& provider, std::optional<PlanLimits> limits,
                   const QString& failure) {
    const auto found = std::find_if(queries_.begin(), queries_.end(),
                                    [&](const Query& query) { return query.provider == provider; });
    if (found == queries_.end())
        return;
    if (auto* process = found->process.data()) {
        disconnect(process, nullptr, this, nullptr);
        stop(process);
    }
    queries_.erase(found);
    if (limits) {
        limits_[provider] = std::move(*limits);
        checked_[provider] = QDateTime::currentDateTime();
    } else if (!failure.isEmpty()) {
        // Keep the last answer; say why it was not refreshed.
        limits_[provider].note = QStringLiteral("Not checked: the CLI %1").arg(failure);
    } else {
        return;
    }
    emit changed();
}

void Usage::count() {
    if (counting_)
        return;
    counting_ = true;
    emit changed();
    const auto day = today();
    const auto since = std::min(QDate(day.year(), day.month(), 1), day.addDays(1 - kChartDays));
    pool_.start([this, since] {
        QThread::currentThread()->setPriority(QThread::LowPriority);
        ledger_.scan(since);
        auto days = ledger_.days();
        QMetaObject::invokeMethod(
            this,
            [this, days = std::move(days)]() mutable {
                days_ = std::move(days);
                counting_ = false;
                if (active_)
                    tokens_timer_.start(kTokensMs);
                emit changed();
            },
            Qt::QueuedConnection);
    });
}

namespace {
// Each window with where it ends if use keeps its pace so far, shown only once
// a tenth of the window has passed.
QVariantList window_list(const PlanLimits& limits, const QDateTime& now) {
    QVariantList windows;
    for (const auto& window : limits.windows) {
        double pace = -1;
        if (window.minutes > 0 && window.resets.isValid()) {
            const double length = window.minutes * 60.0;
            const double elapsed = length - static_cast<double>(now.secsTo(window.resets));
            if (elapsed >= length * 0.1 && elapsed <= length)
                pace = window.percent * length / elapsed;
        }
        windows.append(QVariantMap{{QStringLiteral("label"), window.label},
                                   {QStringLiteral("percent"), window.percent},
                                   {QStringLiteral("resets"), window.resets},
                                   {QStringLiteral("minutes"), window.minutes},
                                   {QStringLiteral("pace"), pace}});
    }
    return windows;
}

TokenCount day_total(const QHash<QString, TokenCount>& models) {
    TokenCount total;
    for (const auto& count : models)
        total += count;
    return total;
}

// Today, this month (and by model, largest first) and the last 30 days.
QVariantMap token_summary(const TokenLedger::Days& days, QDate day) {
    const QDate month(day.year(), day.month(), 1);
    QVariantList chart;
    for (int back = kChartDays - 1; back >= 0; --back) {
        const auto found = days.find(day.addDays(-back));
        chart.append(found == days.end() ? qint64{0} : day_total(found->second).total());
    }
    TokenCount month_count;
    QHash<QString, qint64> models;
    for (auto it = days.lower_bound(month); it != days.end() && it->first <= day; ++it) {
        month_count += day_total(it->second);
        for (auto model = it->second.cbegin(); model != it->second.cend(); ++model)
            models[model.key()] += model->total();
    }
    std::vector<std::pair<QString, qint64>> ranked;
    for (auto it = models.cbegin(); it != models.cend(); ++it)
        ranked.emplace_back(it.key(), it.value());
    std::sort(ranked.begin(), ranked.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    QVariantList model_list;
    for (const auto& [model, total] : ranked)
        model_list.append(
            QVariantMap{{QStringLiteral("name"), model}, {QStringLiteral("total"), total}});
    const auto today = days.find(day);
    return {{QStringLiteral("today"),
             token_map(today == days.end() ? TokenCount{} : day_total(today->second))},
            {QStringLiteral("month"), token_map(month_count)},
            {QStringLiteral("days"), chart},
            {QStringLiteral("models"), model_list}};
}
} // namespace

QVariantList Usage::providers() const {
    QVariantList result;
    const auto now = QDateTime::currentDateTime();
    for (const auto& [id_text, name] : providers_shown) {
        const auto id = QString::fromLatin1(id_text);
        const auto days = days_.value(id);
        if (!limits_.contains(id) && days.empty())
            continue;
        const auto limits = limits_.value(id);
        auto entry = token_summary(days, today());
        entry.insert({{QStringLiteral("id"), id},
                      {QStringLiteral("name"), QString::fromLatin1(name)},
                      {QStringLiteral("plan"), limits.plan},
                      {QStringLiteral("note"), limits.note},
                      {QStringLiteral("checked"), checked_.value(id)},
                      {QStringLiteral("resetCredits"), limits.resetCredits},
                      {QStringLiteral("windows"), window_list(limits, now)}});
        result.append(entry);
    }
    return result;
}
} // namespace lapis::desktop
