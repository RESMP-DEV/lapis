#ifndef LAPIS_DESKTOP_ATTENTION_ORDER_HPP
#define LAPIS_DESKTOP_ATTENTION_ORDER_HPP
#include <QLatin1StringView>
#include <QStringView>

// The order Tab visits agents that need you, shared by the workspace and by
// readers of its published agent state (apps/ultratab). Header-only so a
// reader needs no desktop library.
namespace lapis::desktop {
struct WaitingFacts {
    bool guessed{};     // a next-prompt guess is offered
    bool guess_seen{};  // and it has been on screen
    QStringView status; // SessionPreview::statusKind()
    bool unseen{};      // finished a turn or asked while not shown
    bool request{};     // a request is pending
};
// 0: a guess not yet seen; 1: a turn that finished unseen, or a request; 2: a
// guess already seen (so Tab cannot bounce between two guesses while others
// wait). -1 when the agent does not wait. Within a tier the one waiting
// longest (smallest neededAtMs) comes first.
[[nodiscard]] inline int waiting_tier(const WaitingFacts& facts) {
    const bool waiting_for_prompt =
        facts.status == QLatin1StringView("finished") || facts.status == QLatin1StringView("idle");
    if (facts.guessed && waiting_for_prompt && !facts.guess_seen)
        return 0;
    if (facts.unseen || facts.request)
        return 1;
    if (facts.guessed && waiting_for_prompt)
        return 2;
    return -1;
}
} // namespace lapis::desktop
#endif
