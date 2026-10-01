#include "alerts.hpp"
#include "keymap.hpp"
#include "next_prompt.hpp"
#include "workspace.hpp"
#include <QDateTime>
#include <QStringList>
#include <algorithm>
#include <utility>

namespace lapis::desktop {
SeenScreens::SeenScreens(Workspace& workspace, Looking looking, QObject* parent)
    : QObject(parent), workspace_(workspace), looking_(std::move(looking)) {
    timer_.setInterval(kSampleMs);
    connect(&timer_, &QTimer::timeout, this, &SeenScreens::sample);
    timer_.start();
}

size_t SeenScreens::fingerprint(const SessionPreview* item) {
    auto lines = terminal_screen_text(item->snapshot()).split(QLatin1Char('\n'));
    const auto rule = [&lines](qsizetype row) {
        return lines.at(row).trimmed().startsWith(QStringLiteral("\u2500\u2500\u2500"));
    };
    // Claude Code draws its input box between two rules; keep what is above.
    qsizetype top = -1;
    int rules = 0;
    for (auto row = lines.size() - 1; row >= 0 && rules < 2; --row)
        if (rule(row)) {
            top = row;
            ++rules;
        }
    if (top >= 0)
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
    if (!config_.alertSound() || looking_(item))
        return;
    const auto found =
        std::find_if(waiting_.begin(), waiting_.end(),
                     [item](const Waiting& waiting) { return waiting.item == item; });
    if (found == waiting_.end())
        waiting_.push_back({item, 0});
    else
        found->played = 0; // a new request starts its count again
    if (ring(Chime::needsYou))
        for (auto& waiting : waiting_)
            ++waiting.played;
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
    if (ring(Chime::needsYou))
        for (auto& waiting : waiting_)
            ++waiting.played;
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
    if (!log_ || item == nullptr)
        return;
    log_({{"at", QDateTime::currentDateTime().toString(Qt::ISODateWithMs)},
          {"agent", item->title()},
          {"cli", item->agentName()},
          {"event", QLatin1String(event)},
          {"chime", QLatin1String(decision)}});
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
    if (log_)
        log_({{"at", QDateTime::currentDateTime().toString(Qt::ISODateWithMs)},
              {"agent", item->title()},
              {"cli", item->agentName()},
              {"event", QLatin1String(needsYou ? "needs you" : "finished")},
              {"notification", QLatin1String(skipped != nullptr ? skipped : "posted")}});
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
} // namespace lapis::desktop
