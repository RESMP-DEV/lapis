#include "tab_ranker.hpp"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <cmath>
#include <limits>
#include <utility>

namespace lapis::desktop {
namespace {
constexpr int kLogVersion = 1;
constexpr qint64 kLogLimit = qint64{4} * 1024 * 1024;
constexpr qint64 kRecordLimit = qint64{256} * 1024;
constexpr int kMaxCandidates = 64;
constexpr int kMaxWorkCategories = 32;
constexpr int kMaxCategoryLength = 64;
constexpr double kStaleMinutes = 120.0;
constexpr double kSinceCapMinutes = 3.0 * 24 * 60;

QByteArray line(const QJsonObject& object) {
    return QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
}

QJsonArray toArray(const std::array<double, TabRanker::kFeatures>& values) {
    QJsonArray array;
    for (const double value : values)
        array.append(std::round(value * 1e4) / 1e4);
    return array;
}

QJsonArray loggedCandidates(const std::vector<TabCandidate>& candidates,
                            const std::vector<double>* scores) {
    QJsonArray out;
    const auto rows = TabRanker::features(candidates);
    for (std::size_t c = 0; c < candidates.size() && c < kMaxCandidates; ++c) {
        QJsonObject item{{QStringLiteral("agent"), candidates[c].session},
                         {QStringLiteral("category"), candidates[c].category},
                         {QStringLiteral("x"), toArray(rows[c])}};
        if (candidates[c].guessSeen)
            item.insert(QStringLiteral("guess_seen"), true);
        if (scores)
            item.insert(QStringLiteral("score"), std::round((*scores)[c] * 1e4) / 1e4);
        out.append(item);
    }
    return out;
}
} // namespace

TabAwaySettings parse_tab_away(const QJsonValue& value) {
    TabAwaySettings settings;
    const auto object = value.toObject();
    if (const auto hours = object.value(QStringLiteral("overdueHours")); hours.isDouble())
        settings.overdueHours = std::clamp(hours.toDouble(), 0.0, 24.0 * 7);
    if (const auto work = object.value(QStringLiteral("work")); work.isArray()) {
        settings.work.clear();
        for (const auto& name : work.toArray())
            if (name.isString() && !name.toString().trimmed().isEmpty() &&
                settings.work.size() < kMaxWorkCategories) {
                const auto trimmed = name.toString().trimmed().left(kMaxCategoryLength);
                if (!settings.work.contains(trimmed, Qt::CaseInsensitive))
                    settings.work.append(trimmed);
            }
    }
    return settings;
}

std::vector<std::array<double, TabRanker::kFeatures>>
TabRanker::features(const std::vector<TabCandidate>& candidates) {
    double newest = std::numeric_limits<double>::infinity();
    for (const auto& candidate : candidates)
        newest = std::min(newest, std::max(0.0, candidate.waitMinutes));
    std::vector<std::array<double, kFeatures>> rows;
    rows.reserve(candidates.size());
    for (const auto& c : candidates) {
        const double wait = std::max(0.0, c.waitMinutes);
        rows.push_back({c.work ? 1.0 : 0.0, c.request ? 1.0 : 0.0, c.unseen ? 1.0 : 0.0,
                        c.guess ? 1.0 : 0.0, std::log1p(wait), wait == newest ? 1.0 : 0.0,
                        wait > kStaleMinutes ? 1.0 : 0.0,
                        std::log1p(static_cast<double>(std::max(0, c.turnsLastHour))),
                        std::log1p(std::clamp(c.sinceTurnMinutes, 0.0, kSinceCapMinutes))});
    }
    return rows;
}

TabRanker::TabRanker(QString folder) : folder_(std::move(folder)) {
    if (!folder_.isEmpty())
        log_path_ = QDir(folder_).filePath(QStringLiteral("tab_away.jsonl"));
}

std::vector<double> TabRanker::scores(const std::vector<TabCandidate>& candidates) const {
    std::vector<double> out;
    out.reserve(candidates.size());
    for (const auto& item : candidates) {
        // Overdue first, oldest first; then work, requests, turns not yet
        // seen; the newest within each.
        const auto wait = std::max(0.0, item.waitMinutes);
        if (overdue_minutes_ > 0 && wait > overdue_minutes_ &&
            (item.request || item.unseen || (item.guess && !item.guessSeen))) {
            out.push_back(1e12 + wait);
            continue;
        }
        const bool fresh = item.unseen || (item.guess && !item.guessSeen);
        out.push_back(1e10 * static_cast<double>(item.work) +
                      1e9 * static_cast<double>(item.request) + 1e8 * static_cast<double>(fresh) -
                      std::min(wait, 1e7));
    }
    return out;
}

std::optional<std::size_t> TabRanker::pick(const std::vector<TabCandidate>& candidates,
                                           std::vector<double>* scores_out) const {
    const auto s = scores(candidates);
    std::optional<std::size_t> best;
    for (std::size_t c = 0; c < candidates.size(); ++c)
        if (!best || s[c] > s[*best] ||
            (s[c] == s[*best] && candidates[c].waitMinutes > candidates[*best].waitMinutes))
            best = c;
    if (scores_out)
        *scores_out = s;
    return best;
}

void TabRanker::recordTab(const std::vector<TabCandidate>& candidates,
                          const std::vector<double>& scores, std::size_t picked) const {
    if (scores.size() != candidates.size() || picked >= candidates.size())
        return;
    record({{QStringLiteral("event"), QStringLiteral("tab")},
            {QStringLiteral("picked"), static_cast<int>(picked)},
            {QStringLiteral("candidates"), loggedCandidates(candidates, &scores)}});
}

void TabRanker::recordChoice(const std::vector<TabCandidate>& candidates, std::size_t chosen,
                             bool viaTab) const {
    if (candidates.size() < 2 || candidates.size() > kMaxCandidates || chosen >= candidates.size())
        return;
    record({{QStringLiteral("event"), QStringLiteral("choice")},
            {QStringLiteral("via"), viaTab ? QStringLiteral("tab") : QStringLiteral("person")},
            {QStringLiteral("chosen"), static_cast<int>(chosen)},
            {QStringLiteral("candidates"), loggedCandidates(candidates, nullptr)}});
}

void TabRanker::record(QJsonObject event) const {
    if (log_path_.isEmpty())
        return;
    event.insert(QStringLiteral("v"), kLogVersion);
    event.insert(QStringLiteral("t"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    const auto encoded = line(event);
    if (encoded.size() > kRecordLimit)
        return;
    QFile file(log_path_);
    const QDir directory(folder_);
    if (!directory.mkpath(QStringLiteral("."))) {
        qWarning() << "Tab ranker: cannot create log directory:" << directory.absolutePath();
        return;
    }
    if (file.exists() && file.size() + encoded.size() > kLogLimit) {
        // These are labels (what the person did with each move), the one
        // thing here that cannot be regenerated: the previous file is
        // archived under its date, never deleted.
        const auto previous = log_path_ + QStringLiteral(".1");
        if (QFile::exists(previous) &&
            !QFile::rename(previous, log_path_ + QDateTime::currentDateTimeUtc().toString(
                                                     QStringLiteral("'.'yyyyMMdd-HHmmss")))) {
            qWarning() << "Tab ranker: cannot archive the previous log";
            return;
        }
        if (!file.rename(previous)) {
            qWarning() << "Tab ranker: cannot rotate the log";
            return;
        }
        file.setFileName(log_path_);
    }
    if (!file.open(QIODevice::Append | QIODevice::WriteOnly,
                   QFile::ReadOwner | QFile::WriteOwner) ||
        !file.setPermissions(QFile::ReadOwner | QFile::WriteOwner)) {
        qWarning() << "Tab ranker: cannot write the log:" << file.errorString();
        return;
    }
    if (file.write(encoded) != encoded.size() || !file.flush())
        qWarning() << "Tab ranker: cannot append the log:" << file.errorString();
}

} // namespace lapis::desktop
