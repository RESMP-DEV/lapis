#include "agent_search.hpp"

#include "workspace.hpp"
#include <QVariantMap>
#include <algorithm>

namespace lapis::desktop {
namespace {
constexpr std::array<int, 5> kFieldBonus{12, 6, 6, 4,
                                         6}; // title, place, category, harness, machine

bool boundary(QChar before) {
    return before == QLatin1Char('/') || before == QLatin1Char('-') || before == QLatin1Char('_') ||
           before == QLatin1Char('.') || before == QLatin1Char(' ') || before == QLatin1Char(':') ||
           before == QLatin1Char('~');
}

// The letters of `term` in order within `text` (both lowercased): one point
// a letter, more at word starts and in runs; -1 when they are not all there.
int letters(const QString& term, const QString& text) {
    if (term.isEmpty() || term.size() > text.size())
        return -1;
    int score = 0;
    qsizetype at = 0;
    qsizetype previous = -2;
    for (const QChar letter : term) {
        while (at < text.size() && text[at] != letter)
            ++at;
        if (at == text.size())
            return -1;
        score += 1;
        if (at == 0 || boundary(text[at - 1]))
            score += 8;
        if (at == previous + 1)
            score += 6;
        previous = at;
        ++at;
    }
    if (text.startsWith(term))
        score += 10;
    else if (text.contains(term))
        score += 6;
    return score;
}

QString snippet(const QString& line, qsizetype at, qsizetype length) {
    constexpr qsizetype around = 36;
    const auto start = std::max<qsizetype>(0, at - around);
    const auto end = std::min(line.size(), at + length + around);
    return (start > 0 ? QStringLiteral("…") : QString()) + line.mid(start, end - start).trimmed() +
           (end < line.size() ? QStringLiteral("…") : QString());
}
} // namespace

AgentSearch::AgentSearch(Workspace* workspace, QObject* parent)
    : QObject(parent), workspace_(workspace) {}

void AgentSearch::refresh() {
    if (workspace_ == nullptr)
        return;
    std::vector<AgentSearchEntry> entries;
    for (const auto& value : workspace_->sessions()) {
        const auto* item = qobject_cast<const SessionPreview*>(value.value<QObject*>());
        if (item == nullptr)
            continue;
        const auto where = workspace_->agentPlace(item->sessionId());
        AgentSearchEntry entry{.id = item->sessionId(),
                               .title = item->title(),
                               .place = where.value(QStringLiteral("place")).toString(),
                               .category = where.value(QStringLiteral("category")).toString(),
                               .harness = item->agentName(),
                               .machine = where.value(QStringLiteral("machine")).toString(),
                               .lines = {},
                               .harnessId = item->harnessId()};
        const auto& snapshot = item->snapshot();
        const auto columns = static_cast<std::size_t>(snapshot.size.columns);
        QString row;
        for (std::size_t index = 0; index < snapshot.cells.size(); ++index) {
            const auto cell = snapshot.text(index);
            row += cell.empty()
                       ? QStringLiteral(" ")
                       : QString::fromUcs4(cell.data(), static_cast<qsizetype>(cell.size()));
            if (columns > 0 && (index + 1) % columns == 0) {
                if (!row.trimmed().isEmpty())
                    entry.lines << row.trimmed();
                row.clear();
            }
        }
        entries.push_back(std::move(entry));
    }
    setEntries(entries);
}

void AgentSearch::setEntries(const std::vector<AgentSearchEntry>& entries) {
    prepared_.clear();
    prepared_.reserve(entries.size());
    for (const auto& entry : entries) {
        Prepared prepared{.entry = entry,
                          .fields = {entry.title.toLower(), entry.place.toLower(),
                                     entry.category.toLower(), entry.harness.toLower(),
                                     entry.machine.toLower()},
                          .lines = {}};
        prepared.lines.reserve(entry.lines.size());
        for (const auto& line : entry.lines)
            prepared.lines << line.toLower();
        prepared_.push_back(std::move(prepared));
    }
}

int AgentSearch::score(const Prepared& prepared, const QString& term, QString& found) {
    int best = -1;
    for (std::size_t field = 0; field < prepared.fields.size(); ++field) {
        const int points = letters(term, prepared.fields[field]);
        if (points >= 0)
            best = std::max(best, points + kFieldBonus[field]);
    }
    if (best >= 0)
        return best;
    // The screen only as written: letters scattered across a screen match
    // almost anything.
    for (qsizetype line = 0; line < prepared.lines.size(); ++line) {
        const auto at = prepared.lines[line].indexOf(term);
        if (at < 0)
            continue;
        if (found.isEmpty())
            found = snippet(prepared.entry.lines[line], at, term.size());
        return 4 + static_cast<int>(term.size());
    }
    return -1;
}

std::vector<AgentSearchHit> AgentSearch::rank(const QString& query, int limit) const {
    const auto terms = query.toLower().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    std::vector<AgentSearchHit> hits;
    if (terms.isEmpty()) {
        for (const auto& prepared : prepared_)
            hits.push_back({prepared.entry.id, 0, {}});
        return hits.size() > static_cast<std::size_t>(limit)
                   ? std::vector<AgentSearchHit>(hits.begin(), hits.begin() + limit)
                   : hits;
    }
    for (const auto& prepared : prepared_) {
        int total = 0;
        QString found;
        bool all = true;
        for (const auto& term : terms) {
            const int best = score(prepared, term, found);
            all = best >= 0;
            if (!all)
                break;
            total += best;
        }
        if (all)
            hits.push_back({prepared.entry.id, total, found});
    }
    std::stable_sort(
        hits.begin(), hits.end(),
        [](const AgentSearchHit& a, const AgentSearchHit& b) { return a.score > b.score; });
    if (hits.size() > static_cast<std::size_t>(limit))
        hits.resize(static_cast<std::size_t>(limit));
    return hits;
}

QVariantList AgentSearch::search(const QString& query, int limit) const {
    QVariantList results;
    for (const auto& hit : rank(query, limit)) {
        const auto found =
            std::find_if(prepared_.begin(), prepared_.end(),
                         [&](const Prepared& item) { return item.entry.id == hit.id; });
        if (found == prepared_.end())
            continue;
        const auto& entry = found->entry;
        results.append(QVariantMap{{QStringLiteral("sessionId"), entry.id},
                                   {QStringLiteral("title"), entry.title},
                                   {QStringLiteral("place"), entry.place},
                                   {QStringLiteral("category"), entry.category},
                                   {QStringLiteral("harness"), entry.harness},
                                   {QStringLiteral("machine"), entry.machine},
                                   {QStringLiteral("harnessId"), entry.harnessId},
                                   {QStringLiteral("snippet"), hit.snippet}});
    }
    return results;
}
} // namespace lapis::desktop
