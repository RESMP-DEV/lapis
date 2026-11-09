#ifndef LAPIS_DESKTOP_TAB_RANKER_HPP
#define LAPIS_DESKTOP_TAB_RANKER_HPP

#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>
#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <vector>

namespace lapis::desktop {

// {"tabAway": {"work": ["work"], "overdueHours": 4}}. Tab's order does not
// learn: anything left waiting longer than `overdueHours` (0 turns that off)
// comes first, oldest first, so neglected work cannot sink; then work
// categories, requests, turns not yet seen, and the newest. `work` names the
// categories that count as work; names compare without case. The retired
// `rank` key ("learned" and "fixed" orders) is ignored.
struct TabAwaySettings {
    QStringList work{QStringLiteral("work")};
    double overdueHours{4};
    bool operator==(const TabAwaySettings&) const = default;
};
[[nodiscard]] TabAwaySettings parse_tab_away(const QJsonValue& value);

// One agent that waits on the person when Tab looks for the next one. The
// workspace decides eligibility (a finished turn with no background work in
// flight, or a request); the ranker only orders eligible agents.
struct TabCandidate {
    QString session;
    QString category; // the category's id
    bool work{};
    bool request{};
    bool unseen{};
    bool guess{};     // a guessed next prompt is offered
    bool guessSeen{}; // and the person has seen it
    double waitMinutes{};
    int turnsLastHour{};       // turns it started in the last hour
    double sinceTurnMinutes{}; // since its last turn started; large when never
};

// Orders the agents Tab can move to, and records Tab's moves and where the
// person settled. With a folder, records append to `tab_away.jsonl` there,
// owner-only; without one (previews, tests) nothing is written. Owned by the
// GUI thread.
class TabRanker final {
  public:
    static constexpr std::size_t kFeatures = 9;
    // Each candidate's features as the log records them: work, request,
    // unseen, guess, log wait, newest, stale, log turns in the last hour, log
    // minutes since its last turn.
    [[nodiscard]] static std::vector<std::array<double, kFeatures>>
    features(const std::vector<TabCandidate>& candidates);

    explicit TabRanker(QString folder = {});

    void setOverdueHours(double hours) { overdue_minutes_ = std::max(0.0, hours) * 60.0; }

    // Scores in candidate order (higher first) and the index Tab goes to, or
    // nullopt for no candidates. Ties go to the agent waiting longest.
    [[nodiscard]] std::vector<double> scores(const std::vector<TabCandidate>& candidates) const;
    [[nodiscard]] std::optional<std::size_t> pick(const std::vector<TabCandidate>& candidates,
                                                  std::vector<double>* scores = nullptr) const;
    // Records the move Tab made, with every candidate's features and score.
    void recordTab(const std::vector<TabCandidate>& candidates, const std::vector<double>& scores,
                   std::size_t picked) const;
    // Records where the person settled among the agents that were waiting.
    void recordChoice(const std::vector<TabCandidate>& candidates, std::size_t chosen,
                      bool viaTab) const;

  private:
    void record(QJsonObject event) const;
    double overdue_minutes_{240};
    QString folder_;
    QString log_path_;
};

} // namespace lapis::desktop
#endif
