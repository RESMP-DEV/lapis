#include "factory_state.hpp"

#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>

namespace lapis::desktop::factory {
namespace {
// Judge text is shown on one line of a card; anything longer is cut.
QString line(const QJsonValue& value, qsizetype most = 400) {
    auto text = value.toString().simplified();
    if (text.size() > most)
        text = text.left(most - 1) + QChar(0x2026);
    return text;
}
qint64 millis(const QJsonValue& value) {
    const double number = value.toDouble();
    return number > 0 && number < 9.0e15 ? static_cast<qint64>(number) : 0;
}
} // namespace

QStringList stages() {
    return {QLatin1String(kNeedsYou), QLatin1String(kInProgress), QLatin1String(kLanded),
            QLatin1String(kIdleCull)};
}
bool known_stage(const QString& stage) { return stages().contains(stage); }

State parse(const QByteArray& json) {
    State state;
    QJsonParseError error{};
    const auto document = QJsonDocument::fromJson(json, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        return state;
    const auto root = document.object();
    if (root.contains(QStringLiteral("version")) &&
        root.value(QStringLiteral("version")).toInt() != 1)
        return state;
    state.loaded = true;
    state.updated_at_ms = millis(root.value(QStringLiteral("updatedAtMs")));
    state.judge_model = line(root.value(QStringLiteral("judgeModel")), 80);
    const auto agents = root.value(QStringLiteral("agents")).toObject();
    for (auto it = agents.begin(); it != agents.end() && state.agents.size() < 1024; ++it) {
        if (!it.value().isObject() || it.key().isEmpty() || it.key().size() > 64)
            continue;
        const auto entry = it.value().toObject();
        Verdict verdict;
        verdict.stage = entry.value(QStringLiteral("stage")).toString();
        if (!known_stage(verdict.stage))
            continue;
        // Unsure means keep: only an explicit false recommends closing.
        verdict.keep_needed = entry.value(QStringLiteral("keepNeeded")).toBool(true);
        verdict.headline = line(entry.value(QStringLiteral("headline")));
        verdict.reason = line(entry.value(QStringLiteral("reason")));
        verdict.receipt = line(entry.value(QStringLiteral("receipt")), 120);
        verdict.judged_at_ms = millis(entry.value(QStringLiteral("judgedAtMs")));
        if (verdict.judged_at_ms == 0)
            verdict.judged_at_ms = state.updated_at_ms;
        state.agents.insert(it.key(), verdict);
    }
    return state;
}

State read(const QString& path) {
    if (path.isEmpty())
        return {};
    const QFileInfo info(path);
    if (!info.exists() || info.isSymLink() || !info.isFile() || info.size() > kMaxStateBytes)
        return {};
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return parse(file.read(kMaxStateBytes));
}

QString fallback_stage(const Live& live) {
    if (live.attention_pending || live.status_kind == QLatin1String("waiting"))
        return QLatin1String(kNeedsYou);
    if (live.status_kind == QLatin1String("working") ||
        live.status_kind == QLatin1String("connecting"))
        return QLatin1String(kInProgress);
    if (live.status_kind == QLatin1String("finished") ||
        live.status_kind == QLatin1String("idle")) {
        const bool recent =
            live.finished_at_ms > 0 && live.now_ms - live.finished_at_ms <= kLandedRecentMs;
        // A finished turn with no recorded time ended in this run or just before it.
        if (recent || (live.finished_at_ms == 0 && live.status_kind == QLatin1String("finished")))
            return QLatin1String(kLanded);
    }
    return QLatin1String(kIdleCull);
}

bool verdict_current(const Live& live, const Verdict& verdict) {
    return live.changed_at_ms == 0 || live.changed_at_ms <= verdict.judged_at_ms;
}

QString stage(const Live& live, const Verdict* verdict) {
    // Live ground truth wins: a pending request or a running turn is never
    // overridden by a saved verdict, which may predate this run.
    if (live.attention_pending || live.status_kind == QLatin1String("waiting"))
        return QLatin1String(kNeedsYou);
    if (live.status_kind == QLatin1String("working") ||
        live.status_kind == QLatin1String("connecting"))
        return QLatin1String(kInProgress);
    if (verdict == nullptr || !verdict_current(live, *verdict))
        return fallback_stage(live);
    // The turn is not running, so a mid-turn verdict is stale.
    if (verdict->stage == QLatin1String(kInProgress))
        return fallback_stage(live);
    // A settled verdict holds: the judge may read a question in the reply
    // (needs you) and tells landed work from an idle agent.
    return verdict->stage;
}

} // namespace lapis::desktop::factory
