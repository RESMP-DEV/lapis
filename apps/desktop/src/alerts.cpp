#include "alerts.hpp"
#include "keymap.hpp"
#include "workspace.hpp"
#include <algorithm>
#include <utility>

namespace lapis::desktop {
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
    if (config_.finishSound() && !looking_(item))
        ring(Chime::finished);
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
    if (item == nullptr || !config_.notify() || !background_())
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
