#include "workspace_supervisor.hpp"

#include <QScopedValueRollback>
#include <algorithm>
#include <chrono>
#include <set>
#include <tuple>

namespace lapis::desktop {
namespace {
constexpr std::size_t maximum_sources = 8;
constexpr qsizetype maximum_requests = 128;
constexpr qint64 input_quiet_ms = 1500;
constexpr qint64 manual_cooldown_ms = 3000;
constexpr qint64 dwell_ms = 5000;
constexpr qint64 fairness_ms = 15000;
} // namespace
WorkspaceSupervisor::WorkspaceSupervisor(Workspace& workspace, std::function<qint64()> clock,
                                         QObject* parent)
    : QObject(parent), workspace_(workspace), clock_(std::move(clock)) {
    last_input_ = last_manual_ = last_switch_ = now();
    timer_.setInterval(250);
    connect(&timer_, &QTimer::timeout, this, &WorkspaceSupervisor::tick);
    connect(&workspace_, &Workspace::sessionsChanged, this, &WorkspaceSupervisor::synchronize);
    connect(&workspace_, &Workspace::manualNavigationRequested, this, [this] {
        last_manual_ = now();
        publish();
    });
    connect(&workspace_, &Workspace::interactionChanged, this,
            &WorkspaceSupervisor::noteInteraction);
    connect(&workspace_, &Workspace::focusChanged, this, [this] {
        const auto timestamp = now();
        if (!automatic_focus_)
            last_manual_ = timestamp;
        last_switch_ = timestamp;
        if (const auto* selected = workspace_.focusedSession()) {
            const auto found = sources_.find(selected->sessionId());
            if (found != sources_.end())
                found->second.last_visit = timestamp;
        }
        publish();
    });
    synchronize();
}
WorkspaceSupervisor::~WorkspaceSupervisor() {
    for (const auto& [id, source] : sources_) {
        static_cast<void>(id);
        for (const auto& connection : source.connections)
            disconnect(connection);
    }
}
qint64 WorkspaceSupervisor::now() const {
    const auto timestamp = clock_ ? clock_()
                                  : std::chrono::duration_cast<std::chrono::milliseconds>(
                                        std::chrono::steady_clock::now().time_since_epoch())
                                        .count();
    last_clock_ = std::max(last_clock_, timestamp);
    return last_clock_;
}
void WorkspaceSupervisor::observeSource(const QString& id, SessionPreview* session,
                                        qint64 timestamp) {
    auto found = sources_.find(id);
    if (found != sources_.end() && found->second.session != session) {
        for (const auto& connection : found->second.connections)
            disconnect(connection);
        sources_.erase(found);
        std::erase_if(requests_, [&](const auto& item) { return item.first.first == id; });
    }
    auto [source, inserted] = sources_.try_emplace(id);
    if (inserted) {
        source->second.session = session;
        source->second.last_visit = timestamp;
        source->second.connections = {connect(session, &SessionPreview::attentionChanged, this,
                                              &WorkspaceSupervisor::synchronize),
                                      connect(session, &SessionPreview::connectionChanged, this,
                                              &WorkspaceSupervisor::synchronize)};
    }
}
void WorkspaceSupervisor::synchronize() {
    if (synchronizing_)
        return;
    const QScopedValueRollback guard(synchronizing_, true);
    const auto timestamp = now();
    std::set<QString> live_sources;
    std::set<Key> live_requests;
    for (const auto& value : workspace_.sessions()) {
        auto* session = value.value<SessionPreview*>();
        if (!session || session->sessionId().isEmpty() || live_sources.size() >= maximum_sources)
            continue;
        const auto id = session->sessionId();
        if (!live_sources.insert(id).second)
            continue;
        observeSource(id, session, timestamp);
        const auto rows = session->attentionRequests();
        for (qsizetype index = 0; index < std::min(rows.size(), maximum_requests); ++index) {
            auto row = rows[index].toMap();
            const auto token = row.value(QStringLiteral("token")).toString();
            if (token.isEmpty())
                continue;
            const Key key{id, token};
            live_requests.insert(key);
            auto [request, fresh] = requests_.try_emplace(key);
            if (fresh)
                request->second.first_seen = timestamp;
            row.insert(QStringLiteral("sessionId"), id);
            row.insert(QStringLiteral("sessionTitle"), session->title());
            request->second.row = std::move(row);
        }
    }
    std::erase_if(requests_, [&](const auto& item) { return !live_requests.contains(item.first); });
    for (auto source = sources_.begin(); source != sources_.end();) {
        if (live_sources.contains(source->first)) {
            ++source;
            continue;
        }
        for (const auto& connection : source->second.connections)
            disconnect(connection);
        source = sources_.erase(source);
    }
    if (!sources_.contains(pinned_id_))
        pinned_id_.clear();
    publish();
}
QVariantList WorkspaceSupervisor::attentionQueue() const {
    std::vector<std::pair<Key, const Request*>> ordered;
    ordered.reserve(requests_.size());
    for (const auto& [key, request] : requests_)
        ordered.emplace_back(key, &request);
    std::ranges::sort(ordered, [](const auto& left, const auto& right) {
        return std::tie(left.second->first_seen, left.first) <
               std::tie(right.second->first_seen, right.first);
    });
    QVariantList rows;
    rows.reserve(static_cast<qsizetype>(ordered.size()));
    const auto timestamp = now();
    for (const auto& [key, request] : ordered) {
        static_cast<void>(key);
        auto row = request->row;
        row.insert(QStringLiteral("snoozed"), request->snoozed_until > timestamp);
        rows.append(row);
    }
    return rows;
}
bool WorkspaceSupervisor::review(const QString& sessionId, const QString& token) {
    synchronize();
    const auto source = sources_.find(sessionId);
    if (!requests_.contains({sessionId, token}) || source == sources_.end() ||
        !source->second.session)
        return false;
    noteInteraction();
    emit reviewRequested(source->second.session, token);
    return true;
}
bool WorkspaceSupervisor::snooze(const QString& sessionId, const QString& token, int milliseconds) {
    synchronize();
    const auto found = requests_.find({sessionId, token});
    if (found == requests_.end() || milliseconds <= 0 || milliseconds > 86400000)
        return false;
    found->second.snoozed_until = now() + milliseconds;
    publish();
    return true;
}
bool WorkspaceSupervisor::pinned() const {
    const auto* selected = workspace_.focusedSession();
    return selected && selected->sessionId() == pinned_id_;
}
void WorkspaceSupervisor::setPinned(bool value) {
    const auto* selected = workspace_.focusedSession();
    pinned_id_ = value && selected ? selected->sessionId() : QString{};
    publish();
}
void WorkspaceSupervisor::setEnabled(bool value) {
    if (enabled_ == value)
        return;
    enabled_ = value;
    last_switch_ = now();
    publish();
}
void WorkspaceSupervisor::setPaused(bool value) {
    if (paused_ == value)
        return;
    paused_ = value;
    last_switch_ = now();
    publish();
}
void WorkspaceSupervisor::noteInteraction() {
    last_input_ = now();
    publish();
}
void WorkspaceSupervisor::setWindowActive(bool active) {
    if (window_active_ == active)
        return;
    window_active_ = active;
    noteInteraction();
}
QString WorkspaceSupervisor::status() const {
    if (!enabled_)
        return QStringLiteral("Carousel off");
    if (paused_)
        return QStringLiteral("Carousel paused");
    if (!window_active_)
        return QStringLiteral("Window inactive");
    if (pinned())
        return QStringLiteral("Current session pinned");
    if (workspace_.interactionBlocked() || now() - last_input_ < input_quiet_ms)
        return QStringLiteral("Waiting for input to finish");
    if (now() - last_manual_ < manual_cooldown_ms)
        return QStringLiteral("Manual navigation cooldown");
    if (now() - last_switch_ < dwell_ms)
        return QStringLiteral("Dwelling on current session");
    return QStringLiteral("Carousel running");
}
std::optional<std::pair<bool, qint64>>
WorkspaceSupervisor::requestPriority(const QString& id, qint64 timestamp) const {
    bool has_request = false;
    bool has_unsnoozed = false;
    bool actionable = false;
    qint64 oldest = timestamp;
    for (const auto& [key, request] : requests_) {
        if (key.first != id)
            continue;
        has_request = true;
        if (request.snoozed_until > timestamp)
            continue;
        has_unsnoozed = true;
        if (request.row.value(QStringLiteral("attentionEligible")).toBool() &&
            !request.row.value(QStringLiteral("responding")).toBool()) {
            actionable = true;
            oldest = std::min(oldest, request.first_seen);
        }
    }
    if (has_request && !has_unsnoozed)
        return std::nullopt;
    return std::pair{actionable, oldest};
}
QString WorkspaceSupervisor::candidate(qint64 timestamp) const {
    const auto* current = workspace_.focusedSession();
    if (!current || !current->inputReady())
        return {};
    QString selected;
    std::tuple<int, qint64, qint64, QString> best;
    for (const auto& [id, source] : sources_) {
        const auto* target = source.session.data();
        if (!target || target == current || !target->inputReady() ||
            (target->hasAttentionSource() && !target->attentionReady()))
            continue;
        const auto priority = requestPriority(id, timestamp);
        if (!priority)
            continue;
        const auto [actionable, oldest] = *priority;
        const bool overdue = timestamp - source.last_visit >= fairness_ms;
        const auto score = std::tuple{overdue      ? 0
                                      : actionable ? 1
                                                   : 2,
                                      overdue ? source.last_visit : oldest, source.last_visit, id};
        if (selected.isEmpty() || score < best) {
            best = score;
            selected = id;
        }
    }
    return selected;
}
void WorkspaceSupervisor::tick() {
    synchronize();
    const auto timestamp = now();
    if (!enabled_ || paused_ || pinned() || !window_active_ || workspace_.interactionBlocked() ||
        timestamp - last_input_ < input_quiet_ms || timestamp - last_manual_ < manual_cooldown_ms ||
        timestamp - last_switch_ < dwell_ms)
        return;
    const auto target = candidate(timestamp);
    if (target.isEmpty())
        return;
    const QScopedValueRollback guard(automatic_focus_, true);
    // The workspace rechecks identity/readiness and refuses a pending manual request.
    if (workspace_.focusAutomatically(target))
        last_switch_ = timestamp;
    publish();
}
void WorkspaceSupervisor::publish() {
    const auto timestamp = now();
    const bool snoozing = std::ranges::any_of(
        requests_, [timestamp](const auto& item) { return item.second.snoozed_until > timestamp; });
    const bool polling = (enabled_ && !paused_ && window_active_) || snoozing;
    if (polling && !timer_.isActive())
        timer_.start();
    else if (!polling)
        timer_.stop();
    const QVariantMap state{{QStringLiteral("queue"), attentionQueue()},
                            {QStringLiteral("status"), status()},
                            {QStringLiteral("enabled"), enabled_},
                            {QStringLiteral("paused"), paused_},
                            {QStringLiteral("pinned"), pinned()}};
    if (state == published_state_)
        return;
    published_state_ = state;
    emit changed();
}
} // namespace lapis::desktop
