#include "tab_ranker.hpp"
#include "workspace.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QThread>
#include <QThreadPool>
#include <QVariantMap>
#include <exception>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using lapis::desktop::TabCandidate;
using lapis::desktop::TabRanker;
using lapis::desktop::Workspace;
using lapis::desktop::WorkspaceMode;

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

bool waitFor(const std::function<bool()>& condition, int milliseconds = 5000) {
    QElapsedTimer timer;
    timer.start();
    while (!condition()) {
        if (timer.elapsed() > milliseconds)
            return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
    return true;
}

TabCandidate agent(const char* id, double wait, const char* category) {
    TabCandidate candidate;
    candidate.session = QString::fromLatin1(id);
    candidate.category = QString::fromLatin1(category);
    candidate.waitMinutes = wait;
    candidate.sinceTurnMinutes = wait + 1;
    return candidate;
}

QString picked(const TabRanker& ranker, const std::vector<TabCandidate>& candidates) {
    const auto best = ranker.pick(candidates);
    return best ? candidates[*best].session : QString();
}

// With no choices yet: work first, then a request, then a turn not yet seen,
// then the newest; nothing to pick from is no pick.
void priorOrder() {
    TabRanker ranker;
    require(!ranker.pick({}), "no candidates, no pick");
    auto old = agent("old", 50, "home");
    auto fresh = agent("fresh", 2, "home");
    require(picked(ranker, {old, fresh}) == QLatin1String("fresh"),
            "the prior goes to the newest of equals, not the oldest");
    old.unseen = true;
    require(picked(ranker, {old, fresh}) == QLatin1String("old"),
            "a turn not yet seen goes before one already looked at");
    fresh.request = true;
    require(picked(ranker, {old, fresh}) == QLatin1String("fresh"), "a request goes before it");
    auto work = agent("work", 2, "job");
    work.work = true;
    require(picked(ranker, {old, fresh, work}) == QLatin1String("work"), "work goes first");
    // Weights, not tiers: a work turn already looked at hours ago yields.
    work.waitMinutes = work.sinceTurnMinutes = 300;
    require(picked(ranker, {old, fresh, work}) == QLatin1String("fresh"),
            "a stale work turn does not hold Tab forever");
}

// The earlier fixed order: a guess not yet seen, an unseen turn or request,
// a guess already seen; the longest waiting within each.
void fixedOrder() {
    TabRanker ranker;
    ranker.setLearned(false);
    auto seen_guess = agent("seen", 80, "home");
    seen_guess.guess = seen_guess.guessSeen = true;
    auto unseen = agent("unseen", 5, "home");
    unseen.unseen = true;
    auto older = agent("older", 30, "home");
    older.unseen = true;
    auto guess = agent("guess", 1, "home");
    guess.guess = true;
    auto work = agent("work", 100, "job");
    work.work = true;
    work.guess = work.guessSeen = true;
    require(picked(ranker, {seen_guess, unseen, older, guess, work}) == QLatin1String("guess"),
            "a guess not yet seen goes first");
    require(picked(ranker, {seen_guess, unseen, older, work}) == QLatin1String("older"),
            "then the longest waiting unseen turn");
    require(picked(ranker, {seen_guess, work}) == QLatin1String("work"),
            "then the longest waiting seen guess, work or not");
}

// The ranker follows the person: after they keep choosing a category the
// prior ranks lower, its agents come first.
void learnsOnline() {
    TabRanker ranker;
    auto work = agent("w", 3, "job");
    work.work = true;
    auto games = agent("g", 3, "games");
    require(picked(ranker, {work, games}) == QLatin1String("w"), "work first before learning");
    for (int i = 0; i < 40; ++i) {
        ranker.learn({work, games}, 1, i % 2 == 0);
        require(waitFor([&] { return !ranker.refitting(); }), "the refit finished");
    }
    require(ranker.model().decisions == 40, "every choice was fitted");
    require(ranker.model().categories.value(QStringLiteral("games")) > 0.5,
            "the chosen category gained weight");
    require(picked(ranker, {work, games}) == QLatin1String("g"),
            "after its choices the person's category goes first");
    // Malformed or one-candidate decisions are not learned from.
    ranker.learn({work}, 0, false);
    ranker.learn({work, games}, 5, false);
    require(!ranker.refitting() && ranker.model().decisions == 40, "invalid choices were ignored");
    const auto empty = lapis::desktop::fit_tab_ranker(
        {QJsonObject{{"event", "choice"}, {"chosen", 0}, {"candidates", QJsonArray{}}},
         QJsonObject{{"event", "choice"},
                     {"chosen", 0},
                     {"candidates", QJsonArray{QJsonObject{{"x", QJsonArray{1, 2}}},
                                               QJsonObject{{"x", QJsonArray{1, 2}}}}}}});
    require(empty.decisions == 0 && empty.weights == TabRanker::prior(),
            "a decision with the wrong feature width keeps the prior");
    // Choices made while a fit runs must queue exactly one fit that adopts
    // the newest in-memory decisions before starting again.
    for (int i = 0; i < 5; ++i)
        ranker.learn({work, games}, 1, false);
    require(waitFor([&] { return !ranker.refitting(); }), "the queued refits finished");
    require(ranker.model().decisions == 45, "every queued choice was fitted");
}

// A fit may finish after the ranker is gone. The shared handoff owns only its
// guarded state, never a dangling owner.
void refitSurvivesDestruction() {
    for (int round = 0; round < 8; ++round) {
        TabRanker ranker;
        auto work = agent("w", 3, "job");
        auto games = agent("g", 3, "games");
        ranker.learn({work, games}, 1, false);
    }
    require(QThreadPool::globalInstance()->waitForDone(5000), "retired fits drained");
}

// With a folder, every Tab move and choice is logged owner-only, the fitted
// model is kept owner-only, and a new ranker starts from both.
void persists() {
    QTemporaryDir directory;
    require(directory.isValid(), "temporary folder");
    auto work = agent("w", 3, "job");
    work.work = true;
    const auto games = agent("g", 3, "games");
    {
        TabRanker ranker(directory.path());
        std::vector<double> scores;
        const auto best = ranker.pick({work, games}, &scores).value_or(2);
        require(best == 0 && scores.size() == 2, "scores for every candidate");
        ranker.recordTab({work, games}, scores, best);
        for (int i = 0; i < 40; ++i) {
            ranker.learn({work, games}, 1, false);
            require(waitFor([&] { return !ranker.refitting(); }), "the refit finished");
        }
        require(picked(ranker, {work, games}) == QLatin1String("g"), "learned in place");
    }
    const auto log = QFileInfo(directory.filePath(QStringLiteral("tab_away.jsonl")));
    const auto model = QFileInfo(directory.filePath(QStringLiteral("tab_away_model.json")));
    const auto owner_only = QFile::ReadOwner | QFile::WriteOwner;
    require(log.exists() &&
                (log.permissions() & ~(owner_only | QFile::ReadUser | QFile::WriteUser)) == 0,
            "the decision log is owner-only");
    require(model.exists() &&
                (model.permissions() & ~(owner_only | QFile::ReadUser | QFile::WriteUser)) == 0,
            "the model is owner-only");
    QFile file(log.filePath());
    require(file.open(QIODevice::ReadOnly), "read the log");
    const auto first = QJsonDocument::fromJson(file.readLine()).object();
    const auto candidates = first.value(QStringLiteral("candidates")).toArray();
    require(first.value(QStringLiteral("event")).toString() == QLatin1String("tab") &&
                first.value(QStringLiteral("picked")).toInt() == 0 && candidates.size() == 2 &&
                candidates.at(0).toObject().contains(QStringLiteral("score")) &&
                candidates.at(0).toObject().value(QStringLiteral("x")).toArray().size() ==
                    static_cast<qsizetype>(TabRanker::kFeatures),
            "a Tab move is logged with its candidates' features and scores");
    TabRanker again(directory.path());
    require(picked(again, {work, games}) == QLatin1String("g"),
            "a new ranker starts from the kept model");
    require(waitFor([&] { return !again.refitting(); }), "the startup refit finished");
    require(again.model().decisions == 40, "and refits from the kept choices");
}

// Only agents truly waiting on the person are Tab's to choose, in either
// order: one back at work (a new turn, or a turn paused on its own background
// work, which the observer reports as working) never is, even unseen and
// with a guess; a request always is.
void tabAwayChoosesOnlyAgentsWaitingOnYou() {
    for (const bool learned : {true, false}) {
        Workspace workspace(WorkspaceMode::preview);
        workspace.setTabAway({.learned = learned});
        require(workspace.selectSession(QStringLiteral("renderer")), "select renderer");
        lapis::session::wire::AttentionSnapshot state;
        state.available = state.connected = state.ready = true;
        const auto set = [&](const char* id, lapis::session::attention::Activity activity) {
            auto* item = workspace.session(QString::fromLatin1(id));
            state.activity = activity;
            item->applyAttention(state);
            return item;
        };
        using lapis::session::attention::Activity;
        set("agent", Activity::working);
        auto* busy = set("agent", Activity::turn_completed);
        set("agent", Activity::working); // back at work, still marked unseen
        require(busy->unseen() && busy->statusKind() == QLatin1String("working"),
                "fixture: an unseen agent at work");
        const QVariantMap guesses{{busy->sessionId(), false}};
        require(!workspace.nextPriorityAttention(guesses),
                "an agent at work is never Tab's, unseen and with a guess");
        require(workspace.focusedSession()->sessionId() == QLatin1String("renderer"),
                "Tab with nothing waiting stays");
        require(workspace.session(QStringLiteral("checks"))
                    ->addPreviewRequest(QStringLiteral("r1"), QStringLiteral("approve")),
                "fixture request");
        const auto unavailableRequest = [](const char* id) {
            lapis::session::wire::AttentionSnapshot unavailable;
            unavailable.available = true;
            lapis::session::wire::AttentionItem request;
            request.pending.request.id = std::string(id) + ":approval";
            request.pending.request.reason = "approve";
            unavailable.requests.push_back(request);
            return unavailable;
        };
        auto* stale = workspace.session(QStringLiteral("service"));
        stale->applyAttention(unavailableRequest("stale"));
        stale->invalidateAttention();
        auto* unknown = workspace.session(QStringLiteral("notes"));
        lapis::session::wire::AttentionSnapshot unavailable;
        unavailable = unavailableRequest("unknown");
        unknown->applyAttention(unavailable);
        unknown->invalidateAttention();
        for (auto* unusable : {stale, unknown})
            require(unusable->attentionPending() &&
                        unusable->statusKind() == QLatin1String("unknown"),
                    "fixture: a request without a usable observer");
        require(workspace.nextPriorityAttention(guesses) &&
                    workspace.focusedSession()->sessionId() == QLatin1String("checks"),
                "ended and unknown agents are not request candidates");
        // Restore the focused start for the next ranker mode.
        require(workspace.selectSession(QStringLiteral("renderer")), "return to the start");
        workspace.session(QStringLiteral("checks"))->clearPreviewRequests();
        require(!workspace.nextPriorityAttention(guesses),
                "only the still-unusable requests remain");
        require(workspace.session(QStringLiteral("checks"))
                    ->addPreviewRequest(QStringLiteral("r2"), QStringLiteral("approve")),
                "restore a usable request");
        require(workspace.nextPriorityAttention(guesses) &&
                    workspace.focusedSession()->sessionId() == QLatin1String("checks"),
                "a request is waiting on you");
        require(!workspace.nextPriorityAttention(guesses),
                "Tab never goes back to the agent at work");
    }
}

// The learned default puts a work category first and then follows the
// person: Tab, Tab (the guess is sent and the agent is at work), Tab moves on
// to the next waiting agent, and choices made by hand teach the ranker.
void tabAwayPutsWorkFirstAndLearns() {
    Workspace workspace(WorkspaceMode::preview);
    workspace.setChoiceSettleMsForTesting(20);
    require(workspace.addCategory(QStringLiteral("Work")), "add a work category");
    const auto work = workspace.activeCategoryId();
    require(workspace.moveSession(QStringLiteral("checks"), work), "a work agent");
    require(workspace.selectSession(QStringLiteral("renderer")), "select renderer");
    lapis::session::wire::AttentionSnapshot state;
    state.available = state.connected = state.ready = true;
    using lapis::session::attention::Activity;
    const auto set = [&](const char* id, Activity activity) {
        auto* item = workspace.session(QString::fromLatin1(id));
        state.activity = activity;
        item->applyAttention(state);
        return item;
    };
    const auto finish = [&](const char* id) {
        set(id, Activity::working);
        return set(id, Activity::turn_completed);
    };
    const auto settled = [&](int decisions) {
        return waitFor(
            [&] {
                return workspace.tabRanker().model().decisions == decisions &&
                       !workspace.tabRanker().refitting();
            },
            5000);
    };
    // Tab, Tab: the guess on renderer is sent and it is at work. Tab again
    // moves on, to work before the agent that finished after it.
    set("renderer", Activity::working);
    auto* job = finish("checks");
    QThread::msleep(5);
    auto* other = finish("agent");
    require(workspace.nextPriorityAttention({}) && workspace.focusedSession() == job,
            "Tab goes to the waiting work agent first");
    set("checks", Activity::working); // Tab, Tab: its guess is sent
    require(settled(1), "a prompt sent where Tab went is a choice at once");
    require(workspace.nextPriorityAttention({}) && workspace.focusedSession() == other,
            "Tab moves on to the other waiting agent, never the one at work");
    require(!workspace.nextPriorityAttention({}), "and with nothing else waiting stays");
    require(waitFor([&] { return !workspace.tabRanker().refitting(); }, 5000) &&
                workspace.tabRanker().model().decisions == 1,
            "a move with one agent waiting teaches nothing");
    require(workspace.selectSession(QStringLiteral("renderer")), "back");
    // The person keeps choosing the other agent over waiting work by hand.
    for (int round = 0; round < 24; ++round) {
        require(workspace.selectSession(QStringLiteral("renderer")), "start from renderer");
        finish("checks");
        finish("agent");
        require(workspace.selectSession(QStringLiteral("agent")), "the person goes to agent");
        require(settled(round + 2), "their choice is learned");
    }
    require(workspace.selectSession(QStringLiteral("renderer")), "start from renderer");
    finish("checks");
    finish("agent");
    require(workspace.nextPriorityAttention({}) && workspace.focusedSession() == other,
            "after those choices Tab goes where the person goes");
    workspace.setTabAway({.learned = false});
    require(workspace.selectSession(QStringLiteral("renderer")), "start from renderer");
    finish("agent");
    QThread::msleep(5);
    finish("checks");
    require(workspace.nextPriorityAttention({}) && workspace.focusedSession() == other,
            "the fixed order is still the longest waiting");
}

void settings() {
    using lapis::desktop::parse_tab_away;
    const auto defaults = parse_tab_away(QJsonValue{});
    require(defaults.learned && defaults.work == QStringList{QStringLiteral("work")},
            "learned, with a category named work, by default");
    const auto fixed =
        parse_tab_away(QJsonObject{{"rank", "fixed"}, {"work", QJsonArray{"Job", " ", 3}}});
    require(!fixed.learned && fixed.work == QStringList{QStringLiteral("Job")},
            "fixed rank and named work categories");
}

// An append failure must not pretend the choice was persisted.
void appendFailureLeavesTheLogUnchanged() {
    QTemporaryDir directory;
    require(directory.isValid(), "temporary log directory");
    TabRanker ranker(directory.path());
    auto work = agent("w", 3, "job");
    auto games = agent("g", 3, "games");
    ranker.learn({work, games}, 1, false);
    require(waitFor([&] { return !ranker.refitting(); }), "the first fit finished");
    const auto path = directory.filePath(QStringLiteral("tab_away.jsonl"));
    QFile log(path);
    require(log.open(QIODevice::ReadOnly), "read the first choice");
    const auto bytes = log.readAll();
    log.close();
    require(log.open(QIODevice::ReadWrite) && log.setPermissions(QFile::ReadOwner),
            "make the log append-only failure fixture");
    log.close();
    ranker.learn({work, games}, 1, false);
    require(waitFor([&] { return !ranker.refitting(); }), "the failure refit finished");
    require(log.open(QIODevice::ReadOnly) && log.readAll() == bytes,
            "a failed append leaves the previous log bytes");
    log.close();
    require(log.setPermissions(QFile::ReadOwner | QFile::WriteOwner),
            "restore fixture permissions for cleanup");
}
} // namespace

int main(int argc, char** argv) {
    try {
        QCoreApplication app(argc, argv);
        priorOrder();
        fixedOrder();
        learnsOnline();
        refitSurvivesDestruction();
        persists();
        settings();
        tabAwayChoosesOnlyAgentsWaitingOnYou();
        tabAwayPutsWorkFirstAndLearns();
        appendFailureLeavesTheLogUnchanged();
        std::cout << "tab ranker tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
