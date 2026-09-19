#include <lapis/session/attention.hpp>

#include <iostream>
#include <limits>
#include <source_location>
#include <stdexcept>

namespace {
using namespace lapis::session::attention;
void require(bool value, std::source_location where = std::source_location::current()) {
    if (!value)
        throw std::runtime_error("Attention check failed at line " + std::to_string(where.line()));
}
constexpr auto maximum_sequence = std::numeric_limits<std::uint64_t>::max();
Request request(RequestId id = std::int64_t{1}, std::uint8_t priority = 0) {
    return {std::move(id),  "thread",           "turn",  "item", "approval",
            "Run fixture?", {"accept", "deny"}, priority};
}
State state(const Limits& limits = {}) {
    State result("session", "codex", limits);
    result.connect(1, {true, true, true});
    require(!result.ready());
    require(result.reconcile({1, 0}, {}, 0) == Outcome::applied);
    return result;
}
void identity_and_lifecycle() {
    auto s = state();
    const auto a = request();
    const auto b = request(std::string{"1"});
    require(s.request({1, 1}, a, 0) == Outcome::applied);
    const auto token = s.pending().at(a.id).revision;
    require(s.request({1, 2}, a, 0) == Outcome::duplicate);
    require(s.pending().at(a.id).revision == token);
    require(s.request({1, 3}, b, 0) == Outcome::applied);
    require(s.pending().size() == 2);
    require(s.activity({1, 4}, Activity::turn_completed) == Outcome::applied);
    require(s.pending().size() == 2 && s.connected());
    require(!s.respond(1, a.id, token, "unsupported"));
    require(!s.respond(2, a.id, token, "accept"));
    require(s.respond(1, a.id, token, "accept"));
    require(!s.respond(1, a.id, token, "accept"));
    require(s.pending().size() == 2);
    require(s.resolve({1, 5}, a.id) == Outcome::applied);
    require(s.pending().size() == 1 && s.pending().contains(b.id));
    require(s.resolve({1, 6}, a.id) == Outcome::duplicate);
    require(s.request({1, 7}, a, 0) == Outcome::duplicate);
    require(s.resolve({1, 8}, std::int64_t{44}) == Outcome::applied);
    require(s.request({1, 9}, request(std::int64_t{44}), 0) == Outcome::duplicate);
}
void recovery() {
    auto s = state();
    auto a = request();
    require(s.request({1, 1}, a, 0) == Outcome::applied);
    const auto old = s.pending().at(a.id).revision;
    require(s.request({1, 3}, request(std::int64_t{2}), 0) == Outcome::desynchronized);
    require(!s.ready() && s.ordered(0).empty());
    require(!s.respond(1, a.id, old, "accept"));
    require(s.reconcile({1, 1}, {a}, 0) == Outcome::rejected);
    require(s.reconcile({1, 3}, {a}, 0) == Outcome::applied);
    require(!s.respond(1, a.id, old, "accept"));
    require(s.respond(1, a.id, s.pending().at(a.id).revision, "accept"));
    s.overflow();
    require(s.reconcile({1, 3}, {a}, 0) == Outcome::applied);
    require(!s.respond(1, a.id, s.pending().at(a.id).revision, "accept"));
    s.disconnect();
    require(!s.connected() && s.activity() == Activity::unknown);
    require(s.reconcile({1, 4}, {a}, 0) == Outcome::rejected);
    s.connect(2, {true, true, true});
    require(s.resolve({1, 4}, a.id) == Outcome::rejected);
    require(s.reconcile({2, 0}, {a}, 0) == Outcome::applied);
    require(!s.respond(1, a.id, old, "accept"));
    require(s.respond(2, a.id, s.pending().at(a.id).revision, "deny"));
    require(s.reconcile({2, 1}, {}, 0) == Outcome::applied);
    require(s.request({2, 2}, a, 0) == Outcome::duplicate);
    s.connect(3, {true, false, true});
    require(s.reconcile({3, 0}, {a}, 0) == Outcome::applied);
    require(!s.respond(3, a.id, s.pending().at(a.id).revision, "accept"));
}
void replay_tokens_and_local_eligibility() {
    auto s = state();
    auto a = request();
    auto b = request(std::int64_t{2});
    require(s.request({1, 1}, a, 100) == Outcome::applied);
    require(s.request({1, 2}, b, 100) == Outcome::applied);
    const auto revision = s.pending().at(a.id).revision;
    const auto b_revision = s.pending().at(b.id).revision;
    require(s.snooze(a.id, 300, 100));
    require(s.reconcile({1, 2}, {a, b}, 100) == Outcome::applied);
    require(s.pending().at(a.id).revision == revision);
    require(s.pending().at(a.id).arrived == 100);
    require(s.pending().at(a.id).not_before == 300);
    require(s.ordered(200).front() == b.id);
    require(s.ordered(300).front() == a.id);
    require(s.respond(1, a.id, revision, "accept"));
    const auto submitted_revision = s.pending().at(a.id).revision;
    require(s.reconcile({1, 2}, {a, b}, 300) == Outcome::applied);
    require(s.pending().at(a.id).status == RequestStatus::responding);
    require(s.pending().at(a.id).submitted);
    require(s.pending().at(a.id).revision == submitted_revision);
    require(!s.acknowledge(a.id, 300));
    require(!s.snooze(a.id, 400, 300));

    s.overflow();
    require(!s.acknowledge(b.id, 300));
    require(s.reconcile({1, 3}, {a, b}, 300) == Outcome::applied);
    require(s.pending().at(a.id).revision != revision);
    const auto recovered = s.pending().at(a.id).revision;
    require(!s.respond(1, a.id, recovered, "accept")); // Submitted responses are never retried.
    require(s.pending().at(b.id).revision != b_revision);
    require(!s.respond(1, b.id, b_revision, "accept"));
    require(!s.snooze(a.id, 400, 300));

    s.connect(2, {true, true, true});
    const auto before_epoch = s.pending().at(a.id).revision;
    require(s.reconcile({2, 0}, {a, b}, 300) == Outcome::applied);
    require(s.pending().at(a.id).revision != before_epoch);
    require(s.snooze(a.id, 400, 300)); // A new source epoch is a fresh pending request.
    require(s.respond(2, a.id, s.pending().at(a.id).revision, "accept"));
    require(!s.snooze(a.id, 400, 300));
    s.disconnect();
    require(!s.acknowledge(b.id, 400));
}
void observation_only_requests() {
    auto s = state();
    auto a = request();
    a.choices.clear();
    require(s.request({1, 1}, a, 0) == Outcome::applied);
    const auto revision = s.pending().at(a.id).revision;
    require(s.ordered(0).front() == a.id);
    require(!s.respond(1, a.id, revision, ""));
    require(!s.respond(1, a.id, revision, "accept"));
    require(s.resolve({1, 2}, a.id) == Outcome::applied);
    require(s.pending().empty());
}
void malformed_and_bounds() {
    auto s = state({2, 2, 100, 10});
    auto a = request();
    require(s.request({1, 1}, a, 0) == Outcome::applied);
    a.summary = "Conflicting payload";
    require(s.request({1, 2}, a, 0) == Outcome::desynchronized);
    require(s.pending().at(a.id).request.summary != a.summary);
    require(s.reconcile({1, 2}, {a, a}, 0) == Outcome::desynchronized);
    require(s.pending().size() == 1);
    a.summary = std::string(4097, 'x');
    require(s.reconcile({1, 2}, {a}, 0) == Outcome::desynchronized);
    require(s.reconcile({1, 2}, {}, 0) == Outcome::applied);
    require(s.resolve({1, 3}, std::int64_t{2}) == Outcome::applied);
    require(s.resolve({1, 4}, std::int64_t{3}) == Outcome::desynchronized);
    require(s.reconcile({1, 4}, {}, 0) == Outcome::rejected);
    s.connect(2, {true, true, true});
    require(s.reconcile({2, 0}, {request(), request(std::int64_t{2})}, 0) == Outcome::applied);
    require(s.request({2, 1}, request(std::int64_t{3}), 0) == Outcome::desynchronized);
    require(s.pending().size() == 2);
    require(s.reconcile({2, 1}, {request(std::string{})}, 0) == Outcome::desynchronized);
    require(s.reconcile({2, 1}, {request(std::numeric_limits<std::int64_t>::max())}, 0) ==
            Outcome::applied);
}
void ordering() {
    auto s = state({128, 1024, 100, 100});
    auto a = request();
    auto b = request(std::int64_t{2}, 3);
    require(s.request({1, 1}, a, 0) == Outcome::applied);
    require(s.request({1, 2}, b, 400) == Outcome::applied);
    require(s.ordered(400).front() == a.id); // Aging outranks a newly urgent request.
    require(s.acknowledge(a.id, 400));
    require(s.ordered(400).front() == b.id && s.pending().size() == 2);
    require(s.snooze(b.id, 800, 400));
    require(s.ordered(400).empty());
    require(s.ordered(500).front() == a.id);
    require(s.ordered(800).size() == 2);
    require(!s.snooze(a.id, 399, 400));
    bool rejected{};
    try {
        static_cast<void>(s.ordered(399));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    require(rejected);
}
void recovery_watermark() {
    auto s = state();
    const auto a = request();
    require(s.request({1, 1}, a, 0) == Outcome::applied);
    require(s.request({1, 3}, a, 0) == Outcome::desynchronized);
    require(s.request({1, 4}, a, 0) == Outcome::rejected);
    require(s.reconcile({1, 3}, {a}, 0) == Outcome::rejected);
    require(s.reconcile({1, 4}, {a}, 0) == Outcome::applied);
    require(s.reconcile({1, 6}, {a, a}, 0) == Outcome::desynchronized);
    require(s.reconcile({1, 5}, {a}, 0) == Outcome::rejected);
    require(s.reconcile({1, 6}, {a}, 0) == Outcome::applied);
}
void sequence_exhaustion() {
    auto s = state();
    const auto a = request();
    require(s.reconcile({1, maximum_sequence}, {a}, 0) == Outcome::applied);
    require(s.request({1, maximum_sequence}, request(std::int64_t{2}), 0) == Outcome::duplicate);
}
} // namespace
int main() {
    try {
        identity_and_lifecycle();
        recovery();
        replay_tokens_and_local_eligibility();
        observation_only_requests();
        malformed_and_bounds();
        ordering();
        recovery_watermark();
        sequence_exhaustion();
        std::cout << "Attention identity, lifecycle, recovery, bounds and scheduling passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
