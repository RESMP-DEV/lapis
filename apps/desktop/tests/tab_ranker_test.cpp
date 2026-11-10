#include "tab_ranker.hpp"
#include "workspace.hpp"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QThread>
#include <QVariantMap>
#include <algorithm>
#include <exception>
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

// Overdue first (oldest first), then work, requests, turns not yet seen, and
// the newest; nothing to pick from is no pick.
void plainOrder() {
    TabRanker ranker;
    require(!ranker.pick({}), "no candidates, no pick");
    auto old = agent("old", 50, "home");
    auto fresh = agent("fresh", 2, "home");
    require(picked(ranker, {old, fresh}) == QLatin1String("fresh"), "the newest of equals");
    old.unseen = true;
    require(picked(ranker, {old, fresh}) == QLatin1String("old"), "a turn not yet seen first");
    fresh.request = true;
    require(picked(ranker, {old, fresh}) == QLatin1String("fresh"), "a request before it");
    auto work = agent("work", 200, "job");
    work.work = true;
    require(picked(ranker, {old, fresh, work}) == QLatin1String("work"),
            "work first, however long it waited, short of overdue");
    auto forgotten = agent("forgotten", 300, "home");
    forgotten.unseen = true;
    require(picked(ranker, {old, fresh, work, forgotten}) == QLatin1String("forgotten"),
            "an unseen turn left over four hours goes before everything");
    ranker.setOverdueHours(0);
    require(picked(ranker, {old, fresh, work, forgotten}) == QLatin1String("work"),
            "with overdue off, work leads again");
}

// Clock skew may hand a candidate a negative wait; it cannot make the order
// reward that candidate for waiting less than no time at all.
void negativeWaitsCountAsNone() {
    TabRanker ranker;
    auto behind_clock = agent("skewed", -5, "home");
    behind_clock.unseen = true;
    auto zero = agent("zero", 0, "home");
    zero.unseen = true;
    const auto scores = ranker.scores({behind_clock, zero});
    require(scores.size() == 2 && scores[0] == scores[1],
            "a negative wait scores the same as no wait");
}

// The log parent is created before checking or opening its child.
void createsMissingLogFolder() {
    QTemporaryDir directory;
    require(directory.isValid(), "temporary log parent");
    const auto folder = QDir(directory.path()).filePath(QStringLiteral("nested/tab-away"));
    const TabRanker ranker(folder);
    ranker.recordChoice({agent("w", 3, "job"), agent("g", 3, "games")}, 1, false);
    const QFile log(QDir(folder).filePath(QStringLiteral("tab_away.jsonl")));
    require(log.exists(), "created the missing log folder and wrote its choice");
}

// With a folder, every Tab move and choice is logged owner-only with the
// candidates' features; no model is kept. Malformed choices are not logged.
void logsMovesAndChoices() {
    QTemporaryDir directory;
    require(directory.isValid(), "temporary folder");
    auto work = agent("w", 3, "job");
    work.work = true;
    const auto games = agent("g", 3, "games");
    const TabRanker ranker(directory.path());
    std::vector<double> scores;
    const auto best = ranker.pick({work, games}, &scores).value_or(2);
    require(best == 0 && scores.size() == 2, "scores for every candidate");
    ranker.recordTab({work, games}, scores, best);
    ranker.recordChoice({work, games}, 1, true);
    ranker.recordChoice({work}, 0, false);
    ranker.recordChoice({work, games}, 5, false);
    const auto log = QFileInfo(directory.filePath(QStringLiteral("tab_away.jsonl")));
    const auto owner_only = QFile::ReadOwner | QFile::WriteOwner;
    require(log.exists() &&
                (log.permissions() & ~(owner_only | QFile::ReadUser | QFile::WriteUser)) == 0,
            "the move log is owner-only");
    require(!QFileInfo::exists(directory.filePath(QStringLiteral("tab_away_model.json"))),
            "no model is written");
    QFile file(log.filePath());
    require(file.open(QIODevice::ReadOnly), "read the log");
    const auto lines = file.readAll().trimmed().split('\n');
    require(lines.size() == 2, "one move and one valid choice");
    const auto move = QJsonDocument::fromJson(lines.at(0)).object();
    const auto candidates = move.value(QStringLiteral("candidates")).toArray();
    require(move.value(QStringLiteral("event")).toString() == QLatin1String("tab") &&
                move.value(QStringLiteral("picked")).toInt() == 0 && candidates.size() == 2 &&
                candidates.at(0).toObject().contains(QStringLiteral("score")) &&
                candidates.at(0).toObject().value(QStringLiteral("x")).toArray().size() ==
                    static_cast<qsizetype>(TabRanker::kFeatures),
            "a Tab move is logged with its candidates' features and scores");
    const auto choice = QJsonDocument::fromJson(lines.at(1)).object();
    require(choice.value(QStringLiteral("event")).toString() == QLatin1String("choice") &&
                choice.value(QStringLiteral("chosen")).toInt() == 1 &&
                choice.value(QStringLiteral("via")).toString() == QLatin1String("tab"),
            "a choice is logged with what was chosen and how");
}

// Only agents truly waiting on the person are Tab's to choose: one back at
// work (a new turn, or a turn paused on its own background work, which the
// observer reports as working) never is, even unseen and with a guess; a
// request always is.
void tabAwayChoosesOnlyAgentsWaitingOnYou() {
    Workspace workspace(WorkspaceMode::fixture);
    workspace.setTabAway({});
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
                ->addFixtureRequest(QStringLiteral("r1"), QStringLiteral("approve")),
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
    unknown->applyAttention(unavailableRequest("unknown"));
    unknown->invalidateAttention();
    for (auto* unusable : {stale, unknown})
        require(unusable->attentionPending() && unusable->statusKind() == QLatin1String("unknown"),
                "fixture: a request without a usable observer");
    require(workspace.nextPriorityAttention(guesses) &&
                workspace.focusedSession()->sessionId() == QLatin1String("checks"),
            "ended and unknown agents are not request candidates");
    require(workspace.selectSession(QStringLiteral("renderer")), "return to the start");
    workspace.session(QStringLiteral("checks"))->clearPreviewRequests();
    require(!workspace.nextPriorityAttention(guesses), "only the still-unusable requests remain");
    require(workspace.session(QStringLiteral("checks"))
                ->addFixtureRequest(QStringLiteral("r2"), QStringLiteral("approve")),
            "restore a usable request");
    require(workspace.nextPriorityAttention(guesses) &&
                workspace.focusedSession()->sessionId() == QLatin1String("checks"),
            "a request is waiting on you");
    require(!workspace.nextPriorityAttention(guesses), "Tab never goes back to the agent at work");
}

// A work category comes first: Tab, Tab (the guess is sent and the agent is
// at work), Tab moves on to the next waiting agent, and with nothing else
// waiting stays.
void tabAwayPutsWorkFirst() {
    Workspace workspace(WorkspaceMode::fixture);
    workspace.setTabAway({});
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
    set("renderer", Activity::working);
    auto* job = finish("checks");
    QThread::msleep(5);
    auto* other = finish("agent");
    require(workspace.nextPriorityAttention({}) && workspace.focusedSession() == job,
            "Tab goes to the waiting work agent first, though the other finished later");
    set("checks", Activity::working); // Tab, Tab: its guess is sent
    require(workspace.nextPriorityAttention({}) && workspace.focusedSession() == other,
            "Tab moves on to the other waiting agent, never the one at work");
    require(!workspace.nextPriorityAttention({}), "and with nothing else waiting stays");
}

void settings() {
    using lapis::desktop::parse_tab_away;
    const auto defaults = parse_tab_away(QJsonValue{});
    require(defaults.overdueHours == 4 && defaults.work == QStringList{QStringLiteral("work")},
            "overdue after 4 hours, with a category named work, by default");
    // The retired `rank` key (the learned and fixed orders) is ignored.
    for (const auto* rank : {"learned", "fixed", "plain"})
        require(parse_tab_away(QJsonObject{{"rank", rank}}) == defaults,
                "the retired rank changes nothing");
    const auto off = parse_tab_away(QJsonObject{{"overdueHours", 0}});
    require(off.overdueHours == 0, "overdue off");
    const auto named = parse_tab_away(QJsonObject{{"work", QJsonArray{"Job", " ", 3}}});
    require(named.work == QStringList{QStringLiteral("Job")}, "named work categories");
    QJsonArray many;
    for (int i = 0; i < 40; ++i)
        many.append(QStringLiteral("category-%1").arg(i));
    many.append(QStringLiteral("CATEGORY-1"));
    const auto bounded = parse_tab_away(QJsonObject{{"work", many}});
    require(bounded.work.size() == 32, "category names are bounded");
    require(std::adjacent_find(bounded.work.cbegin(), bounded.work.cend(),
                               [](const QString& first, const QString& second) {
                                   return first.compare(second, Qt::CaseInsensitive) == 0;
                               }) == bounded.work.cend(),
            "duplicate category names are ignored without case");
    const auto truncated =
        parse_tab_away(QJsonObject{{"work", QJsonArray{QStringLiteral("x").repeated(70)}}});
    require(truncated.work.size() == 1 && truncated.work.first().size() == 64,
            "category names have one truncation limit");
}

// An append failure must not pretend the choice was persisted.
void appendFailureLeavesTheLogUnchanged() {
    QTemporaryDir directory;
    require(directory.isValid(), "temporary log directory");
    const TabRanker ranker(directory.path());
    auto work = agent("w", 3, "job");
    auto games = agent("g", 3, "games");
    ranker.recordChoice({work, games}, 1, false);
    const auto path = directory.filePath(QStringLiteral("tab_away.jsonl"));
    QFile log(path);
    require(log.open(QIODevice::ReadOnly), "read the first choice");
    const auto bytes = log.readAll();
    log.close();
    require(log.open(QIODevice::ReadWrite) && log.setPermissions(QFile::ReadOwner),
            "make the log append-only failure fixture");
    log.close();
    ranker.recordChoice({work, games}, 1, false);
    require(log.open(QIODevice::ReadOnly) && log.readAll() == bytes,
            "a failed append leaves the previous log bytes");
    log.close();
    require(log.setPermissions(QFile::ReadOwner | QFile::WriteOwner),
            "restore fixture permissions for cleanup");
}

// Looked at and left: an agent whose finished turn the person stayed on is
// parked until it next begins to wait, so Tab passes it over.
void parkedAgentsWaitForTheirNextTurn() {
    Workspace workspace(WorkspaceMode::fixture);
    workspace.setParkAfterMsForTesting(30);
    lapis::session::wire::AttentionSnapshot state;
    state.available = state.connected = state.ready = true;
    using lapis::session::attention::Activity;
    const auto finish = [&](const char* id) {
        auto* item = workspace.session(QString::fromLatin1(id));
        state.activity = Activity::working;
        item->applyAttention(state);
        state.activity = Activity::turn_completed;
        item->applyAttention(state);
        return item;
    };
    require(workspace.selectSession(QStringLiteral("renderer")), "start on renderer");
    auto* agent_item = finish("agent");
    require(workspace.selectSession(QStringLiteral("agent")), "look at its finished turn");
    QThread::msleep(40);
    require(workspace.selectSession(QStringLiteral("renderer")), "and move on");
    require(workspace.parked(agent_item) && workspace.parkedAtMs(QStringLiteral("agent")) > 0,
            "it is parked");
    require(!workspace.nextPriorityAttention({}), "Tab passes a parked agent over");
    QThread::msleep(5);
    finish("agent");
    require(!workspace.parked(agent_item), "its next turn brings it back");
    require(workspace.nextPriorityAttention({}) && workspace.focusedSession() == agent_item,
            "and Tab goes to it");
    auto* glanced = finish("checks");
    require(workspace.selectSession(QStringLiteral("checks")), "a quick glance");
    require(workspace.selectSession(QStringLiteral("agent")), "straight on");
    require(!workspace.parked(glanced), "a glance does not park");
}
} // namespace

int main(int argc, char** argv) {
    try {
        QCoreApplication app(argc, argv);
        plainOrder();
        negativeWaitsCountAsNone();
        parkedAgentsWaitForTheirNextTurn();
        logsMovesAndChoices();
        createsMissingLogFolder();
        settings();
        tabAwayChoosesOnlyAgentsWaitingOnYou();
        tabAwayPutsWorkFirst();
        appendFailureLeavesTheLogUnchanged();
        std::cout << "tab ranker tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
