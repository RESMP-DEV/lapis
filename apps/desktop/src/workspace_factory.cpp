// The factory view's side of the workspace: the orchestrator agent, links
// that name agents, and every other agent sorted into pipeline stages from
// live state and the judge's factory_state.json (factory_state.hpp).
#include "workspace.hpp"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QUrl>

#include <algorithm>

namespace lapis::desktop {
namespace {
// A settled status: one that says what the agent is doing, not whether its
// connection is up. Only moves between settled statuses count as changes, so
// reconnecting after a restart never makes a judge's verdict stale.
bool settled(const QString& kind) {
    return kind == QLatin1String("working") || kind == QLatin1String("finished") ||
           kind == QLatin1String("idle") || kind == QLatin1String("waiting");
}
int stage_rank(const QString& stage) {
    const auto index = factory::stages().indexOf(stage);
    return index < 0 ? 99 : static_cast<int>(index);
}
} // namespace

void Workspace::setupFactory() {
    // Bursts of status changes publish once, on the next pass of the loop.
    factory_publish_.setSingleShot(true);
    factory_publish_.setInterval(0);
    connect(&factory_publish_, &QTimer::timeout, this, &Workspace::publishFactory);
    connect(this, &Workspace::agentsChanged, this, &Workspace::scheduleFactory);
    connect(this, &Workspace::categoriesChanged, this, &Workspace::scheduleFactory);
    connect(this, &Workspace::focusChanged, this, &Workspace::scheduleFactory);
}

void Workspace::watchFactory(Agent* item) {
    const auto id = item->agentId();
    factory_kind_.insert(id, item->statusKind());
    const auto note = [this, item] {
        const auto& agent_id = item->agentId();
        const auto kind = item->statusKind();
        const auto previous = factory_kind_.value(agent_id);
        if (kind != previous && settled(kind)) {
            // Starting work is always news; otherwise only a move between
            // two settled statuses is, not the first after connecting.
            if (settled(previous) || kind == QLatin1String("working"))
                factory_changed_at_.insert(agent_id, QDateTime::currentMSecsSinceEpoch());
            factory_kind_.insert(agent_id, kind);
        }
        scheduleFactory();
    };
    connect(item, &Agent::statusChanged, this, note);
    // A new request always counts as a change, whatever the status was.
    connect(item, &Agent::attentionArrived, this, [this, item] {
        factory_changed_at_.insert(item->agentId(), QDateTime::currentMSecsSinceEpoch());
        scheduleFactory();
    });
    connect(item, &Agent::attentionChanged, this, &Workspace::scheduleFactory);
    connect(item, &Agent::identityChanged, this, &Workspace::scheduleFactory);
}

void Workspace::scheduleFactory() {
    factory_dirty_ = true;
    if (!factory_publish_.isActive())
        factory_publish_.start();
}

void Workspace::publishFactory() {
    factory_dirty_ = true;
    const auto* home = effectiveHomeAgent();
    const auto home_id = home != nullptr ? home->agentId() : QString{};
    if (home_id != published_home_id_) {
        published_home_id_ = home_id;
        emit homeAgentChanged();
    }
    emit factoryPipelineChanged();
    emit attentionInboxChanged();
}

void Workspace::setFactoryStatePath(const QString& path) {
    factory_state_path_ = path;
    watchFactoryFile();
    reloadFactoryState();
}

void Workspace::watchFactoryFile() {
    delete factory_watcher_;
    factory_watcher_ = nullptr;
    if (factory_state_path_.isEmpty())
        return;
    factory_watcher_ = new QFileSystemWatcher(this);
    // The folder catches the file's creation and an atomic replace, which
    // drops the watch on the file itself.
    factory_watcher_->addPath(QFileInfo(factory_state_path_).absolutePath());
    if (QFileInfo::exists(factory_state_path_))
        factory_watcher_->addPath(factory_state_path_);
    const auto changed = [this] {
        if (factory_watcher_ != nullptr && QFileInfo::exists(factory_state_path_) &&
            !factory_watcher_->files().contains(factory_state_path_))
            factory_watcher_->addPath(factory_state_path_);
        reloadFactoryState();
    };
    connect(factory_watcher_, &QFileSystemWatcher::fileChanged, this, changed);
    connect(factory_watcher_, &QFileSystemWatcher::directoryChanged, this, changed);
}

void Workspace::reloadFactoryState() {
    auto next = factory::read(factory_state_path_);
    if (next.loaded == factory_state_.loaded &&
        next.updated_at_ms == factory_state_.updated_at_ms &&
        next.judge_model == factory_state_.judge_model &&
        next.agents.keys() == factory_state_.agents.keys() && next.loaded) {
        // The folder changes for every file in it; only a new verdict publishes.
        bool same = true;
        for (auto it = next.agents.cbegin(); it != next.agents.cend() && same; ++it) {
            const auto& old = factory_state_.agents[it.key()];
            same = old.stage == it->stage && old.keep_needed == it->keep_needed &&
                   old.headline == it->headline && old.reason == it->reason &&
                   old.receipt == it->receipt && old.judged_at_ms == it->judged_at_ms;
        }
        if (same)
            return;
    }
    if (!next.loaded && !factory_state_.loaded)
        return;
    factory_state_ = std::move(next);
    scheduleFactory();
}

QString Workspace::factoryAutoHome() const {
    const auto agy = [this](const QString& id) {
        const auto entry = agents_.constFind(id);
        return entry != agents_.cend() && entry->harness == QLatin1String("agy") &&
               agentMachine(*entry).isEmpty();
    };
    bool any_agy = false;
    for (const auto& item : items_)
        any_agy = any_agy || agy(item->agentId());
    // A choice made before stays while it is open, unless it was the stage
    // agent and an agy agent has appeared since.
    if (!auto_home_id_.isEmpty() && agent(auto_home_id_) != nullptr &&
        (agy(auto_home_id_) || !any_agy))
        return auto_home_id_;
    QString best;
    qint64 best_at = -1;
    for (const auto& item : items_) {
        const auto& id = item->agentId();
        if (!agy(id))
            continue;
        const auto starts = turn_starts_.value(id);
        const qint64 at = std::max({factory_changed_at_.value(id), waiting_since_.value(id),
                                    starts.empty() ? qint64{0} : starts.back()});
        if (at > best_at) {
            best = id;
            best_at = at;
        }
    }
    if (best.isEmpty() && focusedAgent() != nullptr)
        best = focusedAgent()->agentId();
    auto_home_id_ = best;
    return best;
}

Agent* Workspace::effectiveHomeAgent() const {
    if (!home_agent_id_.isEmpty())
        if (auto* pinned = agent(home_agent_id_))
            return pinned;
    const auto id = factoryAutoHome();
    return id.isEmpty() ? nullptr : agent(id);
}

bool Workspace::setHomeAgent(const QString& agentId) {
    if (!agentId.isEmpty() && agent(agentId) == nullptr)
        return fail(QStringLiteral("That agent is not open."));
    if (home_agent_id_ == agentId)
        return true;
    if (!mutableRegistry())
        return false;
    const auto previous = std::exchange(home_agent_id_, agentId);
    if (!save()) {
        const auto message = error_;
        home_agent_id_ = previous;
        return fail(message);
    }
    publishFactory();
    return true;
}

QString Workspace::resolveAgentLink(const QString& link) const {
    const auto text = link.trimmed();
    QString key;
    bool conversation = false;
    for (const auto* prefix : {"lapis://agent/", "conversation://", "lapis://conversation/"}) {
        const auto scheme = QLatin1String(prefix);
        if (text.startsWith(scheme, Qt::CaseInsensitive)) {
            key = text.mid(scheme.size());
            conversation = !scheme.endsWith(QLatin1String("agent/"));
            break;
        }
    }
    // Only the identifier: no trailing slash, query or fragment.
    key = QUrl::fromPercentEncoding(key.toUtf8());
    key = key.section(QLatin1Char('/'), 0, 0).section(QLatin1Char('?'), 0, 0);
    key = key.section(QLatin1Char('#'), 0, 0).trimmed();
    if (key.isEmpty() || key.size() > 64)
        return {};
    QString found;
    for (const auto& item : items_) {
        const auto& id = item->agentId();
        const auto name = conversation ? agentConversation(id) : id;
        if (name.isEmpty())
            continue;
        if (name.compare(key, Qt::CaseInsensitive) == 0)
            return id;
        if (name.startsWith(key, Qt::CaseInsensitive)) {
            if (!found.isEmpty() && found != id)
                return {}; // a prefix two agents share names neither
            found = id;
        }
    }
    return found;
}

void Workspace::rebuildFactory() const {
    if (!factory_dirty_)
        return;
    factory_dirty_ = false;
    const auto now = QDateTime::currentMSecsSinceEpoch();
    const auto* home = effectiveHomeAgent();
    QHash<QString, QString> category_names;
    for (const auto& category : categories_)
        category_names.insert(category.id, category.name);
    struct Row {
        int rank;
        bool safe; // idle and judged safe to close: first in its stage
        std::size_t order;
        QVariantMap map;
    };
    std::vector<Row> rows;
    struct Waiting {
        qint64 since;
        std::size_t order;
        QVariantMap map;
    };
    std::vector<Waiting> waiting;
    int counts[4] = {0, 0, 0, 0};
    int safe = 0;
    std::size_t order = 0;
    // The sidebar's order: each category's agents in turn.
    for (const auto& category : categories_) {
        for (const auto& item : items_) {
            const auto& id = item->agentId();
            const auto entry = agents_.constFind(id);
            if (entry == agents_.cend() || entry->category != category.id || item.get() == home)
                continue;
            const factory::Live live{.status_kind = item->statusKind(),
                                     .attention_pending = item->attentionPending(),
                                     .changed_at_ms = factory_changed_at_.value(id),
                                     .finished_at_ms = waiting_since_.value(id),
                                     .now_ms = now};
            const auto verdict = factory_state_.agents.constFind(id);
            const auto* judged =
                verdict != factory_state_.agents.cend() ? &verdict.value() : nullptr;
            const auto stage = factory::stage(live, judged);
            // A verdict is shown as current only while it still describes the
            // agent: no change since it was judged and live state agrees with
            // it (a mid-turn verdict on a finished turn, or a settled verdict
            // on a running one, is stale).
            const bool current = judged != nullptr && factory::verdict_current(live, *judged) &&
                                 judged->stage == stage;
            // Unjudged or stale: keep it, since nothing says it can go.
            const bool keep = current ? judged->keep_needed : true;
            const auto ids = item->attentionRequestIds();
            const auto requests = item->attentionRequests();
            QString token =
                requests.isEmpty() ? QString{} : requests.front().toMap().value("token").toString();
            if (token.isEmpty() && !ids.isEmpty())
                token = ids.front();
            QVariantMap map{{"agentId", id},
                            {"shortId", id.left(8)},
                            {"title", item->title()},
                            {"categoryId", category.id},
                            {"categoryName", category.name},
                            {"harnessId", item->harnessId()},
                            {"machine", agentMachine(*entry)},
                            {"statusKind", live.status_kind},
                            {"statusLabel", item->statusLabel()},
                            {"activity", item->activity()},
                            {"attentionPending", live.attention_pending},
                            {"attentionCount", item->attentionCount()},
                            {"attentionReason", item->attentionReason()},
                            {"firstToken", token},
                            {"stage", stage},
                            {"keepNeeded", keep},
                            {"judged", current},
                            {"headline", current ? judged->headline : QString{}},
                            {"reason", current ? judged->reason : QString{}},
                            {"receipt", judged != nullptr ? judged->receipt : QString{}},
                            {"judgedAtMs", judged != nullptr ? judged->judged_at_ms : qint64{0}}};
            const int rank = stage_rank(stage);
            if (rank < 4)
                ++counts[rank];
            const bool closable = stage == QLatin1String(factory::kIdleCull) && current && !keep;
            if (closable)
                ++safe;
            if (live.attention_pending)
                waiting.push_back({waiting_since_.value(id, item->neededAtMs()), order,
                                   QVariantMap{{"agentId", id},
                                               {"shortId", id.left(8)},
                                               {"title", item->title()},
                                               {"categoryName", category.name},
                                               {"harnessId", item->harnessId()},
                                               {"reason", item->attentionReason()},
                                               {"count", item->attentionCount()},
                                               {"firstToken", token}}});
            rows.push_back({rank, closable, order++, std::move(map)});
        }
    }
    std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
        if (a.rank != b.rank)
            return a.rank < b.rank;
        if (a.safe != b.safe)
            return a.safe;
        return a.order < b.order;
    });
    std::stable_sort(waiting.begin(), waiting.end(), [](const Waiting& a, const Waiting& b) {
        return a.since != b.since ? a.since < b.since : a.order < b.order;
    });
    factory_items_.clear();
    for (auto& row : rows)
        factory_items_.append(std::move(row.map));
    attention_inbox_.clear();
    for (auto& item : waiting)
        attention_inbox_.append(std::move(item.map));
    factory_summary_ = {{"needsYou", counts[0]},
                        {"inProgress", counts[1]},
                        {"landed", counts[2]},
                        {"idleCull", counts[3]},
                        {"safeToClose", safe},
                        {"total", static_cast<int>(factory_items_.size())},
                        {"judged", factory_state_.loaded},
                        {"judgeModel", factory_state_.judge_model},
                        {"updatedAtMs", factory_state_.updated_at_ms},
                        {"homeAgentId", home != nullptr ? home->agentId() : QString{}}};
}

QVariantList Workspace::factoryPipeline() const {
    rebuildFactory();
    return factory_items_;
}
QVariantMap Workspace::factorySummary() const {
    rebuildFactory();
    return factory_summary_;
}
QVariantList Workspace::attentionInbox() const {
    rebuildFactory();
    return attention_inbox_;
}

bool Workspace::addFixtureAgent(const QString& id, const QString& title, const QString& category,
                                const QString& harness, const QString& content) {
    if (!fixture_mode_ || id.isEmpty() || agents_.contains(id) ||
        this->category(category) == nullptr)
        return false;
    const auto bytes = content.toUtf8();
    auto item = std::make_unique<Agent>(
        title, QStringLiteral("lapis"), QString{}, QColor(QStringLiteral("#87cbac")),
        std::string_view(bytes.constData(), static_cast<std::size_t>(bytes.size())));
    item->setAgentId(id);
    item->setHarnessId(harness);
    AgentRecord record{category, {}, {}};
    record.harness = harness;
    agents_.insert(id, record);
    items_.push_back(std::move(item));
    watch(items_.back().get());
    changed();
    return true;
}

bool Workspace::setFixtureConversation(const QString& id, const QString& file) {
    if (!fixture_mode_ || !agents_.contains(id))
        return false;
    fixture_files_.insert(id, file);
    return true;
}

} // namespace lapis::desktop
