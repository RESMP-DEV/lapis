#include "alerts.hpp"
#include "keymap.hpp"
#include "next_prompt.hpp"
#include "workspace.hpp"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QStringList>
#include <algorithm>
#include <utility>

namespace lapis::desktop {
namespace {
// One line per decision, chime or notification, in one shape: what was
// decided and why, for the agent and CLI it was about.
void record_decision(const AttentionLog& log, const SessionPreview* item, const char* kind,
                     const char* event, const char* decision) {
    if (!log || item == nullptr)
        return;
    log({{"at", QDateTime::currentDateTime().toString(Qt::ISODateWithMs)},
         {"agent", item->title()},
         {"cli", item->agentName()},
         {"event", QLatin1String(event)},
         {"kind", QLatin1String(kind)},
         {"decision", QLatin1String(decision)}});
}
} // namespace

SeenScreens::SeenScreens(Workspace& workspace, Looking looking, QObject* parent)
    : QObject(parent), workspace_(workspace), looking_(std::move(looking)) {
    timer_.setInterval(kSampleMs);
    connect(&timer_, &QTimer::timeout, this, &SeenScreens::sample);
    timer_.start();
}

size_t SeenScreens::fingerprint(const SessionPreview* item) {
    auto lines = terminal_screen_text(item->snapshot()).split(QLatin1Char('\n'));
    // Claude Code draws its input box between two rules of box-drawing
    // characters; keep what is above. A border may open with a corner or
    // junction glyph or use heavier strokes, so a rule is a line drawn
    // entirely with box-drawing characters. A single rule is a divider in the
    // agent's own output, not the box: only a found pair truncates.
    static const QString drawing = QStringLiteral(
        "\u2500\u2501\u2550\u250c\u2510\u2514\u2518\u251c\u2524\u252c\u2534\u253c\u256d\u256e\u2570\u256f\u255e\u255f\u2560\u2563\u2561\u2562\u256a\u256b\u254b\u254c\u254d\u2504\u2505\u2508\u2509\u2574\u2576\u257a\u257c\u257d\u257e\u257f");
    const auto rule = [&lines](qsizetype row) {
        const auto text = lines.at(row).trimmed();
        return text.size() >= 3 &&
               std::all_of(text.cbegin(), text.cend(), [](QChar glyph) {
                   return drawing.contains(glyph);
               });
    };
    qsizetype top = -1;
    int rules = 0;
    for (auto row = lines.size() - 1; row >= 0 && rules < 2; --row)
        if (rule(row)) {
            top = row;
            ++rules;
        }
    if (rules == 2)
        lines = lines.mid(0, top);
    return qHash(lines.join(QLatin1Char('\n')));
}

void SeenScreens::see(const SessionPreview* item) {
    if (item == nullptr)
        return;
    if (!seen_.contains(item))
        connect(item, &QObject::destroyed, this, [this, item] { seen_.remove(item); });
    seen_.insert(item, fingerprint(item));
}

void SeenScreens::sample() {
    const auto* shown = workspace_.focusedSession();
    if (shown != nullptr && looking_(shown))
        see(shown);
}

bool SeenScreens::unchanged(const SessionPreview* item) const {
    // Null matches see()'s tolerance: an item that was never on screen has
    // nothing seen to compare against, so it counts as changed (the chime
    // plays), the same behavior the signal path had before this class
    // existed. Both turnFinished emission sites pass a live item, but this
    // is the one boundary every caller shares.
    if (item == nullptr)
        return false;
    const auto found = seen_.constFind(item);
    return found != seen_.cend() && *found == fingerprint(item);
}

Alerts::Alerts(Workspace& workspace, const KeyMap& config, Player play, Looking looking,
               QObject* parent)
    : QObject(parent), config_(config), play_(std::move(play)), looking_(std::move(looking)) {
    repeat_.setInterval(kRepeatMs);
    connect(&repeat_, &QTimer::timeout, this, &Alerts::tick);
    connect(&workspace, &Workspace::agentNeedsYou, this, &Alerts::needsYou);
    connect(&workspace, &Workspace::turnFinished, this, &Alerts::finished);
}

void Alerts::preview(bool needsYou) {
    play_(needsYou ? Chime::needsYou : Chime::finished);
    last_.start();
}

bool Alerts::ring(Chime chime) {
    if (last_.isValid() && last_.elapsed() < quiet_ms_)
        return false;
    play_(chime);
    last_.start();
    return true;
}

void Alerts::needsYou(SessionPreview* item) {
    if (!config_.alertSound())
        return record(item, "needs you", "alert off");
    if (looking_(item))
        return record(item, "needs you", "quiet: you are looking at it");
    const auto found =
        std::find_if(waiting_.begin(), waiting_.end(),
                     [item](const Waiting& waiting) { return waiting.item == item; });
    if (found == waiting_.end())
        waiting_.push_back({item, 0});
    else
        found->played = 0; // a new request starts its count again
    const bool chimed = ring(Chime::needsYou);
    if (chimed)
        for (auto& waiting : waiting_)
            ++waiting.played;
    record(item, "needs you", chimed ? "chimed" : "quiet: another chime just played");
    if (!repeat_.isActive())
        repeat_.start();
}

void Alerts::tick() {
    // Answered, looked at, closed, or chimed for enough: done.
    std::erase_if(waiting_, [this](const Waiting& waiting) {
        return !waiting.item || waiting.item->attentionCount() == 0 || looking_(waiting.item) ||
               waiting.played >= config_.alertRepeat();
    });
    if (waiting_.empty() || !config_.alertSound()) {
        waiting_.clear();
        repeat_.stop();
        return;
    }
    // One chime covers every agent still waiting.
    const bool chimed = ring(Chime::needsYou);
    if (chimed)
        for (auto& waiting : waiting_)
            ++waiting.played;
    for (const auto& waiting : waiting_)
        record_decision(log_, waiting.item, "chime", "needs you",
                        chimed ? "chimed" : "quiet: another chime just played");
}

void Alerts::finished(SessionPreview* item) {
    if (!config_.finishSound())
        return record(item, "finished", "chime off");
    if (looking_(item)) {
        if (seen_ != nullptr)
            seen_->see(item);
        return record(item, "finished", "quiet: you are looking at it");
    }
    if (seen_ != nullptr && seen_->unchanged(item))
        return record(item, "finished", "quiet: nothing new since you looked");
    record(item, "finished", ring(Chime::finished) ? "chimed" : "quiet: another chime just played");
}

void Alerts::record(const SessionPreview* item, const char* event, const char* decision) const {
    record_decision(log_, item, "chime", event, decision);
}

Notifier::Notifier(Workspace& workspace, const KeyMap& config, Post post, Background background,
                   QObject* parent)
    : QObject(parent), config_(config), post_(std::move(post)), background_(std::move(background)) {
    connect(&workspace, &Workspace::agentNeedsYou, this,
            [this](SessionPreview* item) { notify(item, true); });
    connect(&workspace, &Workspace::turnFinished, this,
            [this](SessionPreview* item) { notify(item, false); });
}

void Notifier::notify(const SessionPreview* item, bool needsYou) {
    if (item == nullptr)
        return;
    const auto decide = [&]() -> const char* {
        if (!config_.notify())
            return "notifications off";
        if (!background_())
            return "none: lapis is in front";
        if (!needsYou && seen_ != nullptr && seen_->unchanged(item))
            return "none: nothing new since you looked";
        return nullptr;
    };
    const char* skipped = decide();
    record_decision(log_, item, "notification", needsYou ? "needs you" : "finished",
                    skipped != nullptr ? skipped : "posted");
    if (skipped != nullptr)
        return;
    // The CLI leads the body: a title is the conversation's, and one about
    // Codex would otherwise read as a Codex agent.
    const QString cli = item->agentName();
    QString body = needsYou ? tr("%1 needs you").arg(cli) : tr("%1 finished a turn").arg(cli);
    if (needsYou && !item->attentionReason().isEmpty())
        body += QStringLiteral(": ") + item->attentionReason();
    post_(item->sessionId(), item->title(), body);
}

AttentionLog attention_log(const QString& path) {
    return [path](const QJsonObject& line) {
        const auto encoded = QJsonDocument(line).toJson(QJsonDocument::Compact) + '\n';
        QFile file(path);
        if (file.exists() && !file.setPermissions(QFile::ReadOwner | QFile::WriteOwner)) {
            qWarning() << "Attention log: cannot make it private";
            return;
        }
        // Rotate before the line that would cross the cap, so the file never
        // overshoots it, and stop the write when the old log cannot move
        // aside rather than grow without bound. The fresh file opens with a
        // marker line so a reader can tell a rotation from a gap.
        constexpr qint64 cap = qint64{2} * 1024 * 1024;
        if (file.exists() && file.size() + encoded.size() > cap) {
            const auto previous = path + QStringLiteral(".1");
            QFile::remove(previous);
            if (!file.rename(previous)) {
                qWarning() << "Attention log: cannot rotate";
                return;
            }
            file.setFileName(path);
            if (file.open(QIODevice::WriteOnly, QFile::ReadOwner | QFile::WriteOwner)) {
                file.write(QJsonDocument(QJsonObject{
                                 {"at",
                                  QDateTime::currentDateTime().toString(Qt::ISODateWithMs)},
                                 {"event", QStringLiteral("rotated")},
                             })
                               .toJson(QJsonDocument::Compact) +
                           '\n');
                file.close();
            }
        }
        const bool created = !file.exists();
        if (created)
            QDir().mkpath(QFileInfo(path).absolutePath(),
                          QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        if (!file.open(QIODevice::Append | QIODevice::WriteOnly,
                       QFile::ReadOwner | QFile::WriteOwner)) {
            qWarning() << "Attention log: cannot write:" << file.errorString();
            return;
        }
        // Owner-only however it was created: it holds conversation titles.
        if (!file.setPermissions(QFile::ReadOwner | QFile::WriteOwner)) {
            qWarning() << "Attention log: cannot make it private";
            return;
        }
        file.write(encoded);
    };
}
} // namespace lapis::desktop
