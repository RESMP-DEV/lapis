#include <lapis/session/attention.hpp>

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace lapis::session::attention {
namespace {
constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
bool bounded(const std::string& value, std::size_t limit, bool empty = false) {
    return (empty || !value.empty()) && value.size() <= limit;
}
} // namespace
State::State(std::string session_id, std::string adapter_id, const Limits& limits)
    : session_id_(std::move(session_id)), adapter_id_(std::move(adapter_id)), limits_(limits) {
    if (!bounded(session_id_, 256) || !bounded(adapter_id_, 256) || !limits.pending ||
        !limits.retired || !limits.aging_interval)
        throw std::invalid_argument("Invalid attention identity or limits");
}
bool State::valid_id(const RequestId& id) {
    const auto* text = std::get_if<std::string>(&id);
    return !text || bounded(*text, 256);
}
bool State::valid_request(const Request& request) {
    if (!valid_id(request.id) || !bounded(request.thread_id, 256, true) ||
        !bounded(request.turn_id, 256, true) || !bounded(request.item_id, 256, true) ||
        !bounded(request.reason, 256) || !bounded(request.summary, 4096, true) ||
        request.priority > 3 || request.choices.size() > 16)
        return false;
    std::set<std::string> choices;
    for (const auto& choice : request.choices)
        if (!bounded(choice, 256) || !choices.insert(choice).second)
            return false;
    return true;
}
void State::clock(Tick now) {
    if (now < now_)
        throw std::invalid_argument("Attention clock moved backwards");
    now_ = now;
}
std::uint64_t State::revision() {
    if (revision_ == maximum) {
        desynchronize();
        throw std::overflow_error("Attention revision exhausted");
    }
    return ++revision_;
}
void State::desynchronize() {
    synchronized_ = false;
    activity_ = Activity::unknown;
    for (auto& [id, entry] : pending_) {
        static_cast<void>(id);
        entry.status = RequestStatus::stale;
    }
}
void State::connect(std::uint64_t epoch, Capabilities capabilities) {
    if (!epoch || epoch <= epoch_)
        throw std::invalid_argument("Source epochs must increase");
    desynchronize();
    epoch_ = epoch;
    sequence_ = 0;
    capabilities_ = capabilities;
    connected_ = true;
    retired_.clear();
    retired_overflow_ = false;
}
void State::disconnect() {
    connected_ = false;
    desynchronize();
}
void State::overflow() { desynchronize(); }
Outcome State::advance(Position position) {
    if (position.epoch != epoch_ || !connected_)
        return Outcome::rejected;
    if (!ready()) {
        sequence_ = std::max(sequence_, position.sequence);
        return Outcome::rejected;
    }
    if (position.sequence <= sequence_)
        return Outcome::duplicate;
    if (position.sequence != sequence_ + 1) {
        sequence_ = position.sequence;
        desynchronize();
        return Outcome::desynchronized;
    }
    sequence_ = position.sequence;
    return Outcome::applied;
}
Outcome State::reconcile(Position position, const std::vector<Request>& requests, Tick now) {
    clock(now);
    if (position.epoch != epoch_ || !connected_ || !capabilities_.observation ||
        !capabilities_.reconciliation || position.sequence < sequence_ || retired_overflow_)
        return Outcome::rejected;
    sequence_ = position.sequence;
    if (requests.size() > limits_.pending) {
        desynchronize();
        return Outcome::desynchronized;
    }
    const bool already_ready = ready();
    std::map<RequestId, Pending> replacement;
    for (const auto& request : requests) {
        if (!valid_request(request) || replacement.contains(request.id) ||
            retired_.contains(request.id)) {
            desynchronize();
            return Outcome::desynchronized;
        }
        const auto previous = pending_.find(request.id);
        Pending item{request, RequestStatus::pending, 0, now, 0, position.epoch, false};
        if (previous != pending_.end() && previous->second.source_epoch == position.epoch &&
            previous->second.request == request) {
            item.arrived = previous->second.arrived;
            item.not_before = previous->second.not_before;
            if (already_ready)
                item.revision = previous->second.revision;
            if (previous->second.submitted) {
                item.status = RequestStatus::responding;
                item.submitted = true;
            }
        }
        replacement.emplace(request.id, std::move(item));
    }
    auto retired = retired_;
    for (const auto& [id, entry] : pending_)
        if (entry.source_epoch == position.epoch && !replacement.contains(id))
            retired.insert(id);
    if (retired.size() > limits_.retired) {
        retired_overflow_ = true;
        desynchronize();
        return Outcome::desynchronized;
    }
    const auto new_revisions = static_cast<std::uint64_t>(
        std::count_if(replacement.begin(), replacement.end(),
                      [](const auto& entry) { return entry.second.revision == 0; }));
    if (new_revisions > maximum - revision_) {
        desynchronize();
        return Outcome::desynchronized;
    }
    for (auto& [id, entry] : replacement) {
        static_cast<void>(id);
        if (entry.revision == 0)
            entry.revision = revision();
    }
    retired_ = std::move(retired);
    pending_ = std::move(replacement);
    synchronized_ = true;
    return Outcome::applied;
}
Outcome State::request(Position position, const Request& request, Tick now) {
    clock(now);
    const auto result = advance(position);
    if (result != Outcome::applied)
        return result;
    if (!valid_request(request)) {
        desynchronize();
        return Outcome::desynchronized;
    }
    if (retired_.contains(request.id))
        return Outcome::duplicate;
    if (const auto entry = pending_.find(request.id); entry != pending_.end()) {
        if (entry->second.request == request)
            return Outcome::duplicate;
        desynchronize();
        return Outcome::desynchronized;
    }
    if (pending_.size() >= limits_.pending) {
        desynchronize();
        return Outcome::desynchronized;
    }
    pending_.emplace(request.id, Pending{request, RequestStatus::pending, revision(), now, 0,
                                         position.epoch, false});
    return Outcome::applied;
}
Outcome State::resolve(Position position, const RequestId& id) {
    const auto result = advance(position);
    if (result != Outcome::applied)
        return result;
    if (!valid_id(id)) {
        desynchronize();
        return Outcome::desynchronized;
    }
    if (retired_.contains(id))
        return Outcome::duplicate;
    if (retired_.size() >= limits_.retired) {
        retired_overflow_ = true;
        desynchronize();
        return Outcome::desynchronized;
    }
    retired_.insert(id); // Unknown resolutions also prevent later resurrection.
    pending_.erase(id);
    return Outcome::applied;
}
Outcome State::activity(Position position, Activity activity) {
    const auto result = advance(position);
    if (result == Outcome::applied)
        activity_ = activity;
    return result;
}
bool State::respond(std::uint64_t epoch, const RequestId& id, std::uint64_t revision,
                    const std::string& choice) {
    const auto entry = pending_.find(id);
    if (!ready() || !capabilities_.response || epoch != epoch_ || entry == pending_.end())
        return false;
    auto& pending = entry->second;
    if (pending.status != RequestStatus::pending || pending.revision != revision ||
        std::find(pending.request.choices.begin(), pending.request.choices.end(), choice) ==
            pending.request.choices.end())
        return false;
    pending.submitted = true;
    pending.status = RequestStatus::responding;
    pending.revision = this->revision();
    return true;
}
bool State::snooze(const RequestId& id, Tick until, Tick now) {
    clock(now);
    const auto entry = pending_.find(id);
    if (!ready() || entry == pending_.end() || entry->second.status != RequestStatus::pending ||
        until < now)
        return false;
    entry->second.not_before = until;
    return true;
}
bool State::acknowledge(const RequestId& id, Tick now) {
    return snooze(id, now + std::min(limits_.cooldown, maximum - now), now);
}
std::vector<RequestId> State::ordered(Tick now) const {
    if (now < now_)
        throw std::invalid_argument("Attention clock moved backwards");
    struct Ordered {
        RequestId id;
        Tick arrived{};
        std::uint64_t score{};
    };
    std::vector<Ordered> result;
    if (!ready())
        return {};
    for (const auto& [id, entry] : pending_)
        if (entry.status == RequestStatus::pending && entry.not_before <= now)
            result.push_back(
                {id, entry.arrived,
                 std::min((now - entry.arrived) / limits_.aging_interval, maximum - 3) +
                     entry.request.priority});
    std::sort(result.begin(), result.end(), [](const Ordered& left, const Ordered& right) {
        if (left.score != right.score)
            return left.score > right.score;
        return std::tie(left.arrived, left.id) < std::tie(right.arrived, right.id);
    });
    std::vector<RequestId> ids;
    ids.reserve(result.size());
    for (auto& entry : result)
        ids.push_back(std::move(entry.id));
    return ids;
}
} // namespace lapis::session::attention
