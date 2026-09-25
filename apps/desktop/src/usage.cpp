#include "usage.hpp"

#include "count_tokens.hpp"
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSysInfo>
#include <QThread>
#include <QTimeZone>
#include <algorithm>
#include <array>
#include <utility>

namespace lapis::desktop {
namespace {
constexpr qint64 kChunk = qint64{8} << 20;
constexpr qsizetype kLongestCount = qsizetype{64} << 20;
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

QVariantMap token_map(const TokenCount& count) {
    return {{QStringLiteral("total"), count.total()},
            {QStringLiteral("input"), count.input},
            {QStringLiteral("cacheRead"), count.cacheRead},
            {QStringLiteral("cacheWrite"), count.cacheWrite},
            {QStringLiteral("output"), count.output}};
}

constexpr int kWeek = 7 * 24 * 60;
// The CLIs asked on every machine; OMP is asked on this Mac only, since its
// logins are usually shared across machines.
constexpr std::array<const char*, 4> kCliProviders{"codex", "claude", "grok", "kimi"};

QString provider_name(const QString& id) {
    static const QHash<QString, QString> names{
        {QStringLiteral("codex"), QStringLiteral("Codex")},
        {QStringLiteral("claude"), QStringLiteral("Claude")},
        {QStringLiteral("grok"), QStringLiteral("Grok")},
        {QStringLiteral("kimi"), QStringLiteral("Kimi")},
        {QStringLiteral("antigravity"), QStringLiteral("Antigravity")},
        {QStringLiteral("gemini"), QStringLiteral("Gemini")},
        {QStringLiteral("zai"), QStringLiteral("Z.ai")},
        {QStringLiteral("copilot"), QStringLiteral("Copilot")},
        {QStringLiteral("cursor"), QStringLiteral("Cursor")},
    };
    const auto name = names.value(id);
    return name.isEmpty() ? id.left(1).toUpper() + id.mid(1) : name;
}

// A window's name in the meter: 5h, wk, 30d, day; "Week, Fable" is "wk Fable".
QString short_label(const UsageWindow& window) {
    const int minutes = window.minutes;
    QString base = minutes == kWeek     ? QStringLiteral("wk")
                   : minutes == 24 * 60 ? QStringLiteral("day")
                   : minutes > 0 && minutes % (24 * 60) == 0
                       ? QStringLiteral("%1d").arg(minutes / (24 * 60))
                   : minutes > 0 && minutes % 60 == 0 ? QStringLiteral("%1h").arg(minutes / 60)
                                                      : window.label;
    const auto comma = window.label.indexOf(QStringLiteral(", "));
    return comma > 0 && base != window.label ? base + QLatin1Char(' ') + window.label.mid(comma + 2)
                                             : base;
}

const UsageWindow* tightest(const PlanLimits& limits) {
    const auto found = std::max_element(
        limits.windows.begin(), limits.windows.end(),
        [](const UsageWindow& a, const UsageWindow& b) { return a.percent < b.percent; });
    return found == limits.windows.end() ? nullptr : &*found;
}

// An ssh host name as ssh config names it; never an option.
bool valid_host(const QString& host) {
    static const QRegularExpression name(QStringLiteral(R"(^[A-Za-z0-9][A-Za-z0-9._:-]{0,127}$)"));
    return name.match(host).hasMatch();
}
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

QHash<QString, TokenLedger::Days> remote_days(const QJsonObject& counts, QDate since) {
    QHash<QString, TokenLedger::Days> result;
    for (auto provider = counts.begin(); provider != counts.end(); ++provider) {
        auto& days = result[provider.key()];
        const auto models = provider.value().toObject();
        for (auto model = models.begin(); model != models.end(); ++model) {
            const auto hours = model.value().toObject();
            for (auto hour = hours.begin(); hour != hours.end(); ++hour) {
                const auto at =
                    QDateTime::fromString(hour.key() + QStringLiteral(":00:00Z"), Qt::ISODate);
                const auto day = at.toLocalTime().date();
                const auto values = hour.value().toArray();
                if (!at.isValid() || day < since || values.size() != 4)
                    continue;
                days[day][model.key()] += TokenCount{.input = values.at(0).toInteger(),
                                                     .cacheRead = values.at(1).toInteger(),
                                                     .cacheWrite = values.at(2).toInteger(),
                                                     .output = values.at(3).toInteger()};
            }
        }
    }
    return result;
}

Usage::Usage(Programs programs, TokenLedger::Roots roots, QObject* parent)
    : QObject(parent), programs_(std::move(programs)), ledger_(std::move(roots)) {
    pool_.setMaxThreadCount(1);
    machines_.emplace_back();
    connect(&limits_timer_, &QTimer::timeout, this, [this] {
        limits_timer_.setInterval(kLimitsMs);
        askLimits();
    });
    tokens_timer_.setSingleShot(true);
    connect(&tokens_timer_, &QTimer::timeout, this, &Usage::count);
    connect(&remote_timer_, &QTimer::timeout, this, [this] {
        remote_timer_.setInterval(kRemoteTokensMs);
        QStringList hosts;
        for (const auto& machine : machines_)
            if (!machine.host.isEmpty())
                hosts << machine.host;
        for (const auto& host : hosts)
            countRemote(host);
    });
}

Usage::~Usage() {
    setActive(false);
    for (auto& machine : machines_)
        if (auto* counter = machine.counter.data()) {
            counter->disconnect(this);
            counter->kill();
            counter->waitForFinished(1000);
            delete counter;
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
        remote_timer_.start(kFirstRemoteCountMs);
        return;
    }
    limits_timer_.stop();
    tokens_timer_.stop();
    remote_timer_.stop();
    queue_.clear();
    auto running = std::move(running_);
    running_.clear();
    for (auto& check : running)
        delete check.probe.data();
}

void Usage::setMachines(const QStringList& hosts) {
    QStringList wanted;
    for (const auto& host : hosts)
        if (valid_host(host) && !wanted.contains(host))
            wanted << host;
    QStringList current;
    for (auto it = machines_.begin() + 1; it != machines_.end(); ++it)
        current << it->host;
    if (current == wanted)
        return;
    std::vector<Machine> next;
    next.push_back(std::move(machines_.front()));
    QStringList added;
    for (const auto& host : wanted) {
        const auto found =
            std::find_if(machines_.begin() + 1, machines_.end(),
                         [&](const Machine& machine) { return machine.host == host; });
        if (found != machines_.end()) {
            next.push_back(std::move(*found));
            continue;
        }
        Machine machine;
        machine.host = host;
        next.push_back(std::move(machine));
        added << host;
    }
    for (auto it = machines_.begin() + 1; it != machines_.end(); ++it)
        if (!wanted.contains(it->host) && it->counter) {
            it->counter->disconnect(this);
            it->counter->kill();
            it->counter->deleteLater();
        }
    machines_ = std::move(next);
    std::erase_if(queue_, [&](const Check& check) {
        return !check.host.isEmpty() && !wanted.contains(check.host);
    });
    emit changed();
    if (!active_ || added.isEmpty())
        return;
    for (const auto& host : added) {
        for (const auto* provider : kCliProviders)
            queue_.push_back({host, QString::fromLatin1(provider)});
        countRemote(host);
    }
    schedule();
}

void Usage::setMeterOrder(const QStringList& providers) {
    QStringList order;
    for (const auto& provider : providers) {
        const auto id = provider.trimmed().toLower();
        if (!id.isEmpty() && id.size() <= 32 && !order.contains(id))
            order << id;
    }
    if (order == meter_order_)
        return;
    meter_order_ = order;
    emit changed();
}

void Usage::refresh() {
    askLimits();
    count();
    QStringList hosts;
    for (const auto& machine : machines_)
        if (!machine.host.isEmpty())
            hosts << machine.host;
    for (const auto& host : hosts)
        countRemote(host);
}

QDate Usage::today() const { return today_.isValid() ? today_ : QDate::currentDate(); }

QDate Usage::since() const {
    const auto day = today();
    return std::min(QDate(day.year(), day.month(), 1), day.addDays(1 - kChartDays));
}

Usage::Machine* Usage::find(const QString& host) {
    const auto found = std::find_if(machines_.begin(), machines_.end(),
                                    [&](const Machine& machine) { return machine.host == host; });
    return found == machines_.end() ? nullptr : &*found;
}

void Usage::askLimits() {
    const auto queued = [this](const Check& check) {
        const auto same = [&](const Check& other) {
            return other.host == check.host && other.provider == check.provider;
        };
        return std::any_of(queue_.begin(), queue_.end(), same) ||
               std::any_of(running_.begin(), running_.end(),
                           [&](const Running& running) { return same(running.check); });
    };
    for (const auto& machine : machines_) {
        std::vector<Check> checks;
        checks.reserve(kCliProviders.size() + 1);
        for (const auto* provider : kCliProviders)
            checks.push_back({machine.host, QString::fromLatin1(provider)});
        if (machine.host.isEmpty())
            checks.push_back({QString(), QStringLiteral("omp")});
        for (auto& check : checks)
            if (!queued(check))
                queue_.push_back(std::move(check));
    }
    schedule();
}

void Usage::schedule() {
    while (running_.size() < kProbesAtOnce && !queue_.empty()) {
        const auto check = queue_.front();
        queue_.erase(queue_.begin());
        auto* machine = find(check.host);
        if (machine == nullptr)
            continue;
        UsageProbe::Setup setup{.provider = check.provider,
                                .program = check.provider,
                                .host = check.host,
                                .ssh = QString()};
        if (check.host.isEmpty()) {
            setup.program = programs_ ? programs_(check.provider) : QString();
            if (setup.program.isEmpty()) {
                // Not installed here.
                if (check.provider == QLatin1String("omp"))
                    machine->accounts.clear();
                else
                    machine->logins.remove(check.provider);
                emit changed();
                continue;
            }
        } else {
            setup.ssh = programs_ ? programs_(QStringLiteral("ssh")) : QString();
            if (setup.ssh.isEmpty())
                continue;
        }
        auto* probe = new UsageProbe(
            setup, [this, check](const UsageProbe::Result& result) { probed(check, result); },
            this);
        running_.push_back({check, probe});
        probe->start();
    }
}

void Usage::probed(const Check& check, const UsageProbe::Result& result) {
    const auto running = std::find_if(running_.begin(), running_.end(), [&](const Running& item) {
        return item.check.host == check.host && item.check.provider == check.provider;
    });
    if (running != running_.end()) {
        if (running->probe)
            running->probe->deleteLater();
        running_.erase(running);
    }
    QTimer::singleShot(0, this, &Usage::schedule);
    auto* machine = find(check.host);
    if (machine == nullptr)
        return;
    if (result.failure == QLatin1String("could not connect")) {
        machine->note = QStringLiteral("Could not connect over ssh");
    } else if (!check.host.isEmpty()) {
        machine->note.clear();
    }
    if (check.provider == QLatin1String("omp")) {
        if (result.limits)
            machine->accounts = *result.limits;
        else if (result.absent)
            machine->accounts.clear();
    } else if (result.limits && !result.absent) {
        machine->logins[check.provider] = result.limits->front();
        machine->checked[check.provider] = QDateTime::currentDateTime();
    } else if (result.absent) {
        machine->logins.remove(check.provider);
    } else if (machine->logins.contains(check.provider)) {
        // Keep the last answer; say why it was not refreshed.
        machine->logins[check.provider].note =
            QStringLiteral("Not checked: the CLI %1").arg(result.failure);
    }
    emit changed();
}

void Usage::count() {
    if (counting_)
        return;
    counting_ = true;
    emit changed();
    pool_.start([this, since = since()] {
        QThread::currentThread()->setPriority(QThread::LowPriority);
        ledger_.scan(since);
        auto days = ledger_.days();
        QMetaObject::invokeMethod(
            this,
            [this, days = std::move(days)]() mutable {
                machines_.front().days = std::move(days);
                counting_ = false;
                if (active_)
                    tokens_timer_.start(kTokensMs);
                emit changed();
            },
            Qt::QueuedConnection);
    });
}

// Another machine counts its own transcripts with python3, niced, at most
// every five minutes; this Mac only reads the totals.
void Usage::countRemote(const QString& host) {
    auto* machine = find(host);
    const auto ssh = programs_ ? programs_(QStringLiteral("ssh")) : QString();
    if (machine == nullptr || host.isEmpty() || machine->counter || ssh.isEmpty() ||
        (machine->counted.isValid() && machine->counted.secsTo(QDateTime::currentDateTime()) < 300))
        return;
    const auto start = QDateTime(since(), QTime(0, 0)).toSecsSinceEpoch();
    auto* process = new QProcess(this);
    process->setProgram(ssh);
    process->setArguments(
        {QStringLiteral("-T"), QStringLiteral("-o"), QStringLiteral("BatchMode=yes"),
         QStringLiteral("-o"), QStringLiteral("ConnectTimeout=8"), host,
         QStringLiteral(R"(cd /tmp 2>/dev/null; exec "${SHELL:-/bin/sh}" -lc %1)")
             .arg(shell_words({QStringLiteral("exec nice -n 19 python3 - %1").arg(start)}))});
    process->setStandardErrorFile(QProcess::nullDevice());
    machine->counter = process;
    machine->output.clear();
    connect(process, &QProcess::readyReadStandardOutput, this, [this, host, process] {
        auto* counted = find(host);
        if (counted == nullptr)
            return;
        counted->output += process->readAllStandardOutput();
        if (counted->output.size() > kLongestCount)
            process->kill();
    });
    connect(process, &QProcess::finished, this, [this, host, process](int code) {
        process->deleteLater();
        auto* counted = find(host);
        if (counted == nullptr)
            return;
        counted->counter.clear();
        const auto totals = QJsonDocument::fromJson(counted->output).object();
        counted->output.clear();
        if (code == 0 && !totals.isEmpty()) {
            counted->days = remote_days(totals, since());
            counted->counted = QDateTime::currentDateTime();
        }
        emit changed();
    });
    QTimer::singleShot(kRemoteCountMs, process, [process] { process->kill(); });
    process->start();
    process->write(kCountTokensScript);
    process->closeWriteChannel();
    emit changed();
}

bool Usage::counting() const {
    return counting_ || std::any_of(machines_.begin(), machines_.end(), [](const Machine& machine) {
               return !machine.counter.isNull();
           });
}

// Plans first in the usual order, then any other OMP knows, by name.
QStringList Usage::providerOrder(const Machine& machine) const {
    QStringList ids;
    for (auto it = machine.logins.cbegin(); it != machine.logins.cend(); ++it)
        ids << it.key();
    for (const auto& account : machine.accounts)
        ids << account.provider;
    for (auto it = machine.days.cbegin(); it != machine.days.cend(); ++it)
        if (!it->empty())
            ids << it.key();
    ids.removeDuplicates();
    std::sort(ids.begin(), ids.end(), [](const QString& a, const QString& b) {
        const auto rank = [](const QString& id) {
            const auto found =
                std::find_if(kCliProviders.begin(), kCliProviders.end(),
                             [&](const char* known) { return id == QLatin1String(known); });
            return static_cast<int>(found - kCliProviders.begin());
        };
        return rank(a) != rank(b) ? rank(a) < rank(b) : a < b;
    });
    return ids;
}

QVariantMap Usage::providerEntry(const Machine& machine, const QString& id,
                                 const QDateTime& now) const {
    QVariantList accounts;
    const auto login = machine.logins.constFind(id);
    const bool signed_in = login != machine.logins.cend();
    if (signed_in)
        accounts.append(
            QVariantMap{{QStringLiteral("label"), QStringLiteral("Signed in to the CLI")},
                        {QStringLiteral("source"), login->source},
                        {QStringLiteral("plan"), login->plan},
                        {QStringLiteral("note"), login->note},
                        {QStringLiteral("resetCredits"), login->resetCredits},
                        {QStringLiteral("checked"), machine.checked.value(id)},
                        {QStringLiteral("windows"), window_list(*login, now)}});
    for (const auto& account : machine.accounts) {
        if (account.provider != id || (signed_in && same_account(account, *login)))
            continue;
        accounts.append(QVariantMap{{QStringLiteral("label"), account.account.isEmpty()
                                                                  ? QStringLiteral("An OMP account")
                                                                  : account.account},
                                    {QStringLiteral("source"), account.source},
                                    {QStringLiteral("plan"), account.plan},
                                    {QStringLiteral("note"), account.note},
                                    {QStringLiteral("resetCredits"), account.resetCredits},
                                    {QStringLiteral("checked"), QDateTime()},
                                    {QStringLiteral("windows"), window_list(account, now)}});
    }
    auto entry = token_summary(machine.days.value(id), today());
    entry.insert({{QStringLiteral("id"), id},
                  {QStringLiteral("name"), provider_name(id)},
                  {QStringLiteral("counted"), machine.days.contains(id)},
                  {QStringLiteral("accounts"), accounts}});
    return entry;
}

QVariantList Usage::machines() const {
#ifdef Q_OS_MACOS
    const auto here = QStringLiteral("This Mac");
#else
    const auto here = QStringLiteral("This computer");
#endif
    const auto now = QDateTime::currentDateTime();
    QVariantList result;
    for (const auto& machine : machines_) {
        QVariantList providers;
        for (const auto& id : providerOrder(machine))
            providers.append(providerEntry(machine, id, now));
        result.append(
            QVariantMap{{QStringLiteral("host"), machine.host},
                        {QStringLiteral("name"), machine.host.isEmpty() ? here : machine.host},
                        {QStringLiteral("note"), machine.note},
                        {QStringLiteral("counting"),
                         machine.host.isEmpty() ? counting_ : !machine.counter.isNull()},
                        {QStringLiteral("providers"), providers}});
    }
    return result;
}

// The CLI's own sign-in when there is one, since new agents use it;
// otherwise the OMP account with the most room left.
QVariantList Usage::meter() const {
    const auto& here = machines_.front();
    QVariantList rows;
    for (const auto& id : meter_order_.isEmpty() ? providerOrder(here) : meter_order_) {
        const PlanLimits* chosen = nullptr;
        if (const auto login = here.logins.constFind(id);
            login != here.logins.cend() && !login->windows.empty())
            chosen = &*login;
        const auto room = [](const PlanLimits& limits) {
            const auto* window = tightest(limits);
            return window == nullptr ? 101.0 : window->percent;
        };
        for (const auto& account : here.accounts)
            if (account.provider == id &&
                (chosen == nullptr ||
                 (chosen->source == QLatin1String("omp") && room(account) < room(*chosen))))
                chosen = &account;
        const auto* window = chosen ? tightest(*chosen) : nullptr;
        if (window == nullptr)
            continue;
        rows.append(QVariantMap{{QStringLiteral("id"), id},
                                {QStringLiteral("name"), provider_name(id)},
                                {QStringLiteral("percent"), window->percent},
                                {QStringLiteral("label"), short_label(*window)}});
    }
    return rows;
}
} // namespace lapis::desktop
