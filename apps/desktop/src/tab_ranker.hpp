#ifndef LAPIS_DESKTOP_TAB_RANKER_HPP
#define LAPIS_DESKTOP_TAB_RANKER_HPP

#include <QHash>
#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
#include <QString>
#include <QStringList>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace lapis::desktop {

// {"tabAway": {"rank": "learned", "work": ["work"]}}. `rank` is "learned"
// (the default: a prior that learns from where the person goes) or "fixed"
// (the earlier order: a guess not yet seen, then an unseen turn or a request,
// then a guess already seen, longest waiting first). `work` names the
// categories that count as work, which the prior puts first; names compare
// without case.
struct TabAwaySettings {
    bool learned{true};
    QStringList work{QStringLiteral("work")};
    bool operator==(const TabAwaySettings&) const = default;
};
[[nodiscard]] TabAwaySettings parse_tab_away(const QJsonValue& value);

// One agent that waits on the person when Tab looks for the next one. The
// workspace decides eligibility (a finished turn with no background work in
// flight, or a request); the ranker only orders eligible agents.
struct TabCandidate {
    QString session;
    QString category; // the category's id: learned weights follow it across renames
    bool work{};
    bool request{};
    bool unseen{};
    bool guess{};     // a guessed next prompt is offered
    bool guessSeen{}; // and the person has seen it
    double waitMinutes{};
    int turnsLastHour{};       // turns it started in the last hour
    double sinceTurnMinutes{}; // since its last turn started; large when never
};

// Orders the agents Tab can move to, and learns that order from the person.
// Scores are a linear softmax ranker over a few features plus one weight per
// category, pulled toward a prior (work first, then requests, then turns not
// yet seen, then the newest) and fitted to every recorded choice: each time
// the person settles on a waiting agent, by Tab or by hand, the waiting agents
// at the moment they left the previous one and the one they chose.
//
// Owned by the GUI thread. Scoring is a few dot products; refitting runs on
// the thread pool from the decision log and publishes new weights back here.
// With a folder, decisions append to `tab_away.jsonl` there and the fitted
// model is kept in `tab_away_model.json`, both owner-only; without one
// (previews, tests) decisions stay in memory.
class TabRanker final : public QObject {
    Q_OBJECT
  public:
    static constexpr std::size_t kFeatures = 9;
    using Weights = std::array<double, kFeatures>;
    struct Model {
        Weights weights{};
        QHash<QString, double> categories;
        int decisions{};
    };
    // Feature order: work, request, unseen, guess, log wait, newest, stale,
    // log turns in the last hour, log minutes since its last turn.
    [[nodiscard]] static Weights prior();
    [[nodiscard]] static std::vector<std::array<double, kFeatures>>
    features(const std::vector<TabCandidate>& candidates);

    explicit TabRanker(QString folder = {}, QObject* parent = nullptr);
    ~TabRanker() override;
    TabRanker(const TabRanker&) = delete;
    TabRanker& operator=(const TabRanker&) = delete;
    TabRanker(TabRanker&&) = delete;
    TabRanker& operator=(TabRanker&&) = delete;

    void setLearned(bool learned) { learned_ = learned; }
    [[nodiscard]] bool learned() const { return learned_; }
    [[nodiscard]] const Model& model() const { return model_; }

    // Scores in candidate order (higher first) and the index Tab goes to, or
    // nullopt for no candidates. Ties go to the agent waiting longest.
    [[nodiscard]] std::vector<double> scores(const std::vector<TabCandidate>& candidates) const;
    [[nodiscard]] std::optional<std::size_t> pick(const std::vector<TabCandidate>& candidates,
                                                  std::vector<double>* scores = nullptr) const;
    // Records the move Tab made, with every candidate's features and score.
    void recordTab(const std::vector<TabCandidate>& candidates, const std::vector<double>& scores,
                   std::size_t picked);
    // Records where the person settled and refits in the background.
    void learn(const std::vector<TabCandidate>& candidates, std::size_t chosen, bool viaTab);

    // Tests: whether a refit is running or queued, and the decision bound.
    [[nodiscard]] bool refitting() const;
    static constexpr int kMaxDecisions = 1000;
  signals:
    void modelChanged();

  private:
    struct Shared;
    void record(QJsonObject event) const;
    void refit();
    void adopt(Model model);
    bool learned_{true};
    QString folder_;
    QString log_path_;
    QString model_path_;
    Model model_;
    std::shared_ptr<Shared> shared_;
};

// The model fitted to a set of decisions (each as logged: candidates with
// features, and the chosen index), regularized toward the prior. Pure; runs on
// a worker thread.
[[nodiscard]] TabRanker::Model fit_tab_ranker(const std::vector<QJsonObject>& decisions);

} // namespace lapis::desktop
#endif
