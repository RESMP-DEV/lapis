#ifndef LAPIS_SESSION_ATTENTION_HPP
#define LAPIS_SESSION_ATTENTION_HPP

#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <variant>
#include <vector>

namespace lapis::session::attention {
inline constexpr std::uint32_t contract_version = 1;
using RequestId = std::variant<std::int64_t, std::string>;
using Tick = std::uint64_t; // Caller-supplied monotonic milliseconds.
enum class Activity : std::uint8_t { unknown, working, idle, turn_completed };
enum class Outcome : std::uint8_t { applied, duplicate, rejected, desynchronized };
enum class RequestStatus : std::uint8_t { pending, responding, stale };
struct Position {
    std::uint64_t epoch;
    std::uint64_t sequence;
};
struct Capabilities {
    bool observation{};
    bool response{};
    bool reconciliation{};
};
struct Limits {
    std::size_t pending{128};
    std::size_t retired{1024};
    Tick aging_interval{1000};
    Tick cooldown{1000}; // Zero disables the acknowledgement delay.
};
struct Request {
    RequestId id;
    std::string thread_id;
    std::string turn_id;
    std::string item_id;
    std::string reason;
    std::string summary;
    // Empty means an observation-only notice; the adapter must resolve it from source state.
    std::vector<std::string> choices;
    std::uint8_t priority{}; // 0..3; aging eventually outranks a newer high priority.
    bool operator==(const Request&) const = default;
};
struct Pending {
    Request request;
    RequestStatus status{RequestStatus::pending};
    std::uint64_t revision{};
    Tick arrived{};
    Tick not_before{};
    std::uint64_t source_epoch{};
    bool submitted{};
};

// One service-owned source connection. Single-thread ownership; no I/O or focus policy.
// Sequences order local delivery only. The adapter must separately establish source completeness.
class State {
  public:
    // Throws std::invalid_argument for invalid identities or limits.
    State(std::string session_id, std::string adapter_id, const Limits& limits = {});
    // Throws std::invalid_argument for a zero or non-increasing source epoch.
    void connect(std::uint64_t epoch, Capabilities capabilities);
    void disconnect();
    void overflow();
    // Start a fresh, observation-only ledger at a verified source boundary.
    // This cannot restore a lost stream or replace pending requests. It grants
    // neither response nor authoritative reconciliation capabilities.
    Outcome begin_observation(Position position, Tick now);
    // Authoritative snapshots replace state atomically. Retired IDs persist for an epoch;
    // exhausting their bound requires a new epoch, never silent tombstone eviction.
    // A content-identical same-epoch entry retains its revision only when the state is
    // already synchronized; recovery, desynchronization, changed content, and new epochs
    // issue fresh revisions. Methods taking Tick throw std::invalid_argument for a backward
    // clock. Token allocation can throw std::overflow_error when revisions are exhausted.
    // Sequence wrap is not observed as a gap: duplicates remain duplicates, and the caller
    // must establish a new epoch before sequence UINT64_MAX would be exceeded.
    Outcome reconcile(Position position, const std::vector<Request>& requests, Tick now);
    Outcome request(Position position, const Request& request, Tick now);
    Outcome resolve(Position position, const RequestId& id);
    Outcome activity(Position position, Activity activity);
    // Exact source/epoch/revision plus adapter-validated choice. Sending never retires a request.
    // A lost response must not be retransmitted automatically by the adapter.
    // request()/respond() throw std::overflow_error if token space is exhausted;
    // reconcile() instead reports desynchronized when its token batch cannot fit.
    [[nodiscard]] bool respond(std::uint64_t epoch, const RequestId& id, std::uint64_t revision,
                               const std::string& choice);
    // Local scheduling accepts only synchronized, pending requests. It never resolves them.
    [[nodiscard]] bool snooze(const RequestId& id, Tick until, Tick now);
    [[nodiscard]] bool acknowledge(const RequestId& id, Tick now);
    [[nodiscard]] std::vector<RequestId> ordered(Tick now) const;
    [[nodiscard]] const std::map<RequestId, Pending>& pending() const { return pending_; }
    [[nodiscard]] bool ready() const { return connected_ && synchronized_; }
    [[nodiscard]] bool connected() const { return connected_; }
    [[nodiscard]] Activity activity() const { return activity_; }
    [[nodiscard]] std::uint64_t epoch() const { return epoch_; }
    [[nodiscard]] const std::string& session_id() const { return session_id_; }
    [[nodiscard]] const std::string& adapter_id() const { return adapter_id_; }

  private:
    static bool valid_id(const RequestId& id);
    static bool valid_request(const Request& request);
    Outcome advance(Position position);
    void desynchronize();
    void clock(Tick now);
    std::uint64_t revision();
    std::string session_id_;
    std::string adapter_id_;
    Limits limits_;
    Capabilities capabilities_;
    std::uint64_t epoch_{};
    std::uint64_t sequence_{};
    std::uint64_t revision_{};
    Tick now_{};
    bool connected_{};
    bool synchronized_{};
    bool retired_overflow_{};
    Activity activity_{Activity::unknown};
    std::map<RequestId, Pending> pending_;
    std::set<RequestId> retired_;
};
} // namespace lapis::session::attention
#endif
