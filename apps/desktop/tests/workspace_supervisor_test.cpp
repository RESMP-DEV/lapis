#include "workspace_supervisor.hpp"
#include <QCoreApplication>
#include <QEventLoop>
#include <QJsonObject>
#include <algorithm>
#include <iostream>
#include <source_location>
#include <stdexcept>

namespace {
using namespace lapis::desktop;
namespace wire = lapis::session::wire;
namespace attention = lapis::session::attention;
void require(bool ok, std::source_location where = std::source_location::current()) {
    if (!ok)
        throw std::runtime_error("Supervisor check failed at line " + std::to_string(where.line()));
}
wire::AttentionItem request(attention::RequestId id, std::uint64_t revision = 1) {
    wire::AttentionItem item;
    item.pending.request = {.id = std::move(id),
                            .thread_id = "thread",
                            .turn_id = "turn",
                            .item_id = "item",
                            .reason = "Approval",
                            .summary = "Fixture",
                            .choices = {"accept"}};
    item.pending.source_epoch = 1;
    item.pending.revision = revision;
    return item;
}
wire::AttentionSnapshot snapshot(std::vector<wire::AttentionItem> requests) {
    wire::AttentionSnapshot state;
    state.attachment = {{QByteArray(16, 's'), QByteArray(16, 'e')}, 1};
    state.available = state.connected = state.ready = true;
    state.source_epoch = 1;
    state.requests = std::move(requests);
    return state;
}
QString token(SessionPreview& session, qsizetype index = 0) {
    return session.attentionRequests()[index].toMap().value("token").toString();
}
struct Fixture {
    qint64 time{100000};
    Workspace workspace{WorkspaceMode::preview};
    WorkspaceSupervisor supervisor{workspace, [this] { return time; }};
    SessionPreview* first{workspace.session(QStringLiteral("shell"))};
    SessionPreview* second{workspace.session(QStringLiteral("renderer"))};
    Fixture() {
        first->setConnection(QStringLiteral("ready"), true);
        second->setConnection(QStringLiteral("ready"), true);
    }
    void advance(qint64 milliseconds) {
        time += milliseconds;
        supervisor.tick();
    }
    void enable() {
        supervisor.setEnabled(true);
        supervisor.setWindowActive(true);
    }
    void at(SessionPreview* expected,
            std::source_location where = std::source_location::current()) {
        require(workspace.focusedSession() == expected, where);
    }
};
void aggregation() {
    Fixture f;
    auto state = snapshot({request(std::int64_t{7}), request(std::string{"7"})});
    f.first->applyAttention(state);
    f.second->applyAttention(snapshot({request(std::int64_t{7})}));
    require(!f.supervisor.enabled() && f.supervisor.pendingCount() == 3);
    const auto integer = token(*f.first);
    const auto text = token(*f.first, 1);
    require(integer != text && integer == token(*f.second));
    const auto before = f.supervisor.attentionQueue();
    f.first->applyAttention(state);
    require(f.supervisor.attentionQueue() == before);
    SessionPreview* reviewed = nullptr;
    QObject observation;
    QObject::connect(&f.supervisor, &WorkspaceSupervisor::reviewRequested, &observation,
                     [&](SessionPreview* source, const QString&) { reviewed = source; });
    require(f.supervisor.review(f.second->sessionId(), integer));
    require(reviewed == f.second);
    require(f.supervisor.review(f.first->sessionId(), integer));
    require(reviewed == f.first);
    require(!f.supervisor.review(QStringLiteral("removed"), integer));
    f.at(f.first);
    f.first->invalidateAttention();
    const auto rows = f.supervisor.attentionQueue();
    require(rows.size() == 3);
    for (const auto& value : rows) {
        const auto row = value.toMap();
        require(row.value("enabled").toBool() ==
                (row.value("sessionId").toString() == f.second->sessionId()));
    }
    state.requests.erase(state.requests.begin());
    state.requests.front().pending.revision = 9;
    state.requests.front().pending.source_epoch = state.source_epoch = 2;
    f.first->applyAttention(state);
    require(f.supervisor.pendingCount() == 2);
    require(!f.supervisor.review(f.first->sessionId(), integer));
    require(!f.supervisor.review(f.first->sessionId(), text));
    require(f.supervisor.review(f.second->sessionId(), integer));
    auto responding = snapshot({request(std::int64_t{7})});
    responding.requests[0].pending.submitted = true;
    f.second->applyAttention(responding);
    require(std::ranges::any_of(f.supervisor.attentionQueue(), [](const auto& value) {
        return value.toMap().value("responding").toBool();
    }));
    f.first->applyAttention(snapshot({}));
    f.second->applyAttention(snapshot({}));
    require(f.supervisor.pendingCount() == 0);
    require(!f.supervisor.snooze(f.second->sessionId(), integer, 1000));
}
void scheduled_carousel_switches_without_manual_ticks() {
    Fixture f;
    QEventLoop loop;
    QObject::connect(&f.workspace, &Workspace::focusChanged, &loop, &QEventLoop::quit);
    QTimer::singleShot(2000, &loop, &QEventLoop::quit);
    f.enable();
    f.time += 5001;
    loop.exec();
    f.at(f.second);
}
void unchanged_ticks_preserve_queue_delegates() {
    Fixture f;
    f.first->applyAttention(snapshot({request(std::int64_t{1})}));
    int updates = 0;
    QObject observation;
    QObject::connect(&f.supervisor, &WorkspaceSupervisor::changed, &observation,
                     [&] { ++updates; });
    f.supervisor.tick();
    f.supervisor.tick();
    require(updates == 0);
    require(f.supervisor.snooze(f.first->sessionId(), token(*f.first), 1000));
    require(updates == 1);
    f.supervisor.tick();
    require(updates == 1);
}
void bounds_and_identity_replacement() {
    Fixture f;
    for (const auto& value : f.workspace.sessions()) {
        auto* source = value.value<SessionPreview*>();
        std::vector<wire::AttentionItem> requests;
        requests.reserve(129);
        for (std::int64_t id = 0; id < 129; ++id)
            requests.push_back(request(id));
        source->applyAttention(snapshot(requests));
    }
    require(f.supervisor.pendingCount() == 128 * f.workspace.sessions().size());
    const auto old_id = f.second->sessionId();
    const auto old_token = token(*f.second);
    f.second->setSessionId(QStringLiteral("replacement"));
    require(!f.supervisor.review(old_id, old_token));
    require(f.supervisor.review(f.second->sessionId(), old_token));
    require(f.supervisor.pendingCount() == 128 * f.workspace.sessions().size());
}
void activation_pause_pin_and_snooze() {
    Fixture f;
    f.advance(20000);
    f.at(f.first);
    f.supervisor.setEnabled(true);
    f.advance(20000);
    f.at(f.first); // Inactive by construction.
    f.supervisor.setWindowActive(true);
    f.advance(1499);
    f.at(f.first);
    f.advance(1);
    f.at(f.second);
    f.supervisor.setPaused(true);
    f.advance(20000);
    f.at(f.second);
    f.supervisor.setPaused(false);
    f.advance(4999);
    f.at(f.second);
    f.advance(1);
    f.at(f.first);
    f.supervisor.setPinned(true);
    f.advance(20000);
    f.at(f.first);
    f.workspace.setFocusedIndex(1);
    f.at(f.second);
    require(!f.supervisor.pinned());
    f.supervisor.setPinned(false);
    f.first->applyAttention(snapshot({request(std::int64_t{1})}));
    require(!f.supervisor.snooze(f.first->sessionId(), token(*f.first), 0));
    require(f.supervisor.snooze(f.first->sessionId(), token(*f.first), 30000));
    f.advance(29999);
    f.at(f.second);
    f.advance(1);
    f.at(f.first);
    f.supervisor.setEnabled(false);
    f.advance(50000);
    f.at(f.first);
}
void interaction_and_fairness() {
    Fixture f;
    f.enable();
    f.advance(4999);
    f.at(f.first);
    f.advance(1);
    f.at(f.second);
    f.supervisor.noteInteraction();
    f.advance(1499);
    f.at(f.second);
    f.advance(3501);
    f.at(f.first);
    f.workspace.setInteractionBlocked(QStringLiteral("composition"), true);
    f.advance(20000);
    f.at(f.first);
    f.workspace.setInteractionBlocked(QStringLiteral("composition"), false);
    f.advance(1499);
    f.at(f.first);
    f.advance(1);
    f.at(f.second);
    f.workspace.setFocusedIndex(0);
    f.advance(5000);
    f.at(f.second);
    f.advance(5000);
    f.at(f.first);
    f.workspace.setFocusedIndex(0); // Same-owner manual intent still starts cooldown.
    f.advance(2999);
    f.at(f.first);
    f.advance(2001);
    f.at(f.second);
    f.first->setConnection(QStringLiteral("disconnected"), false);
    f.advance(20000);
    f.at(f.second);
    f.first->setConnection(QStringLiteral("ready"), true);
    f.supervisor.tick();
    f.at(f.first);
    f.second->applyAttention(snapshot({request(std::int64_t{1})}));
    auto* quiet = f.workspace.session(QStringLiteral("agent"));
    quiet->setConnection(QStringLiteral("ready"), true);
    bool visited = false;
    for (int step = 0; step < 3; ++step) {
        f.advance(5000);
        visited |= f.workspace.focusedSession() == quiet;
    }
    require(visited); // Continually pending neighbor cannot starve an eligible quiet session.
}
} // namespace
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        aggregation();
        scheduled_carousel_switches_without_manual_ticks();
        unchanged_ticks_preserve_queue_delegates();
        bounds_and_identity_replacement();
        activation_pause_pin_and_snooze();
        interaction_and_fairness();
        std::cout
            << "Workspace queue identity, recovery, bounds and guarded fair carousel passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
