#ifndef LAPIS_SUPERVISOR_STATE_HPP
#define LAPIS_SUPERVISOR_STATE_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace lapis::supervisor {

// The first supervisor state schema is deliberately one session slot. Values
// are UTF-8 text; identifiers are lowercase hexadecimal without punctuation.
inline constexpr std::size_t max_state_bytes = 8 * 1024;
inline constexpr std::size_t max_control_bytes = 8 * 1024;
inline constexpr std::size_t max_endpoint_bytes = 4096;
inline constexpr std::size_t max_blocked_reason_bytes = 512;
inline constexpr std::size_t epoch_hex_bytes = 32;
inline constexpr std::size_t identity_hex_bytes = 32;
inline constexpr std::size_t fingerprint_hex_bytes = 64;
inline constexpr std::size_t spawn_token_hex_bytes = 64;

enum class DesiredState : std::uint8_t { stopped = 0, started = 1 };

struct SessionIdentity {
    std::string session_id;
    std::string epoch;
    bool operator==(const SessionIdentity&) const = default;
};

struct DesiredSession {
    std::string endpoint;
    std::string fingerprint;
    SessionIdentity identity;
    DesiredState desired_state{DesiredState::stopped};
    std::string spawn_token;
    std::string blocked_reason;
    bool operator==(const DesiredSession&) const = default;
};

struct SupervisorState {
    int version{1};
    bool enabled{false};
    std::string instance_epoch;
    std::optional<DesiredSession> session;
    bool operator==(const SupervisorState&) const = default;
};

enum class ControlAction : std::uint8_t { start = 0, stop = 1, disable = 2 };

struct ControlRequest {
    std::string instance_epoch;
    std::string token;
    ControlAction action{ControlAction::start};
    // A start's endpoint, fingerprint and identity. It is not the state
    // record yet: the registry adds desired state and a fresh spawn token.
    std::optional<DesiredSession> session;
    bool operator==(const ControlRequest&) const = default;
};

enum class ControlStatus : std::uint8_t {
    applied = 0,
    unauthorized = 1,
    stale_epoch = 2,
    rejected = 3
};

struct ControlOutcome {
    ControlStatus status{ControlStatus::rejected};
    SupervisorState state;
    bool operator==(const ControlOutcome&) const = default;
};

[[nodiscard]] bool valid_hex(const std::string& value, std::size_t length);
[[nodiscard]] bool valid_state(const SupervisorState& state);
[[nodiscard]] bool valid_desired_session(const DesiredSession& session);
[[nodiscard]] bool constant_time_equal(const std::string& trusted, const std::string& supplied);

class StateCodec {
  public:
    virtual ~StateCodec() = default;
    StateCodec(const StateCodec&) = delete;
    StateCodec& operator=(const StateCodec&) = delete;

    [[nodiscard]] virtual std::string encode(const SupervisorState& state) const = 0;
    [[nodiscard]] virtual SupervisorState decode(const std::string& bytes) const = 0;
    // Malformed or oversized requests throw. Authentication and transition
    // semantics remain in the registry, not in this parser.
    [[nodiscard]] virtual ControlRequest decode_control(const std::string& bytes) const = 0;

  protected:
    StateCodec() = default;
};

class JsonStateCodec final : public StateCodec {
  public:
    [[nodiscard]] std::string encode(const SupervisorState& state) const override;
    [[nodiscard]] SupervisorState decode(const std::string& bytes) const override;
    [[nodiscard]] ControlRequest decode_control(const std::string& bytes) const override;
};

class StateStorage {
  public:
    virtual ~StateStorage() = default;
    StateStorage(const StateStorage&) = delete;
    StateStorage& operator=(const StateStorage&) = delete;

    // A second acquisition before release is a caller error. release() is
    // called by destruction of the authoritative registry that acquired it.
    virtual void acquire() = 0;
    virtual void release() = 0;
    [[nodiscard]] virtual std::optional<std::string> load() const = 0;
    virtual void store(const std::string& bytes) = 0;

  protected:
    StateStorage() = default;
};

class MemoryStateStorage final : public StateStorage {
  public:
    explicit MemoryStateStorage(std::optional<std::string> bytes = std::nullopt);
    void acquire() override;
    void release() override;
    [[nodiscard]] std::optional<std::string> load() const override;
    void store(const std::string& bytes) override;

    [[nodiscard]] unsigned write_count() const noexcept { return write_count_; }
    [[nodiscard]] bool fsynced() const noexcept { return fsynced_; }
    [[nodiscard]] bool mode_is_owner_only() const noexcept { return owner_only_; }

  private:
    std::optional<std::string> bytes_;
    bool locked_{false};
    unsigned write_count_{0};
    bool fsynced_{false};
    bool owner_only_{false};
};

// POSIX-backed singleton storage. Constructing it validates the private
// directory but does not block; acquire() opens and holds supervisor.lock.
class PosixStateStorage final : public StateStorage {
  public:
    explicit PosixStateStorage(std::string directory);
    ~PosixStateStorage() override;

    void acquire() override;
    void release() override;
    [[nodiscard]] std::optional<std::string> load() const override;
    void store(const std::string& bytes) override;

  private:
    struct State;
    std::unique_ptr<State> state_;
};

class IdentityProvider {
  public:
    virtual ~IdentityProvider() = default;
    IdentityProvider(const IdentityProvider&) = delete;
    IdentityProvider& operator=(const IdentityProvider&) = delete;

    [[nodiscard]] virtual std::string instance_epoch() = 0;
    [[nodiscard]] virtual std::string session_id() = 0;
    [[nodiscard]] virtual std::string session_epoch() = 0;
    [[nodiscard]] virtual std::string spawn_token() = 0;

  protected:
    IdentityProvider() = default;
};

// Single-threaded authoritative state owner. Storage owns the singleton lock;
// blocking filesystem work belongs to that owner's I/O context.
//
// A failed persistence commit poisons the registry: it keeps the last known
// in-memory state and refuses every later transition or control request. A
// commit can rename the state file and then fail its directory fsync, so what
// is on disk is unknown from inside the process; only a new registry, built
// after this one released the lock, may decide from storage again.
class SupervisorRegistry final {
  public:
    SupervisorRegistry(std::shared_ptr<StateStorage> storage, std::shared_ptr<StateCodec> codec,
                       std::shared_ptr<IdentityProvider> identities);
    ~SupervisorRegistry();
    SupervisorRegistry(const SupervisorRegistry&) = delete;
    SupervisorRegistry& operator=(const SupervisorRegistry&) = delete;

    [[nodiscard]] const SupervisorState& state() const noexcept { return state_; }
    // Direct authoritative transitions are local operations. Remote requests
    // must pass through control(); no production wiring is included here.
    [[nodiscard]] SupervisorState start(DesiredSession session);
    [[nodiscard]] SupervisorState restart(DesiredSession session);
    [[nodiscard]] SupervisorState stop(const std::string& blocked_reason = {});
    [[nodiscard]] SupervisorState disable();
    [[nodiscard]] ControlOutcome control(const ControlRequest& request);

  private:
    void commit(SupervisorState next);
    [[nodiscard]] DesiredSession start_record(DesiredSession session) const;
    [[nodiscard]] DesiredSession restart_record(DesiredSession session) const;

    std::shared_ptr<StateStorage> storage_;
    std::shared_ptr<StateCodec> codec_;
    std::shared_ptr<IdentityProvider> identities_;
    SupervisorState state_;
    bool poisoned_{false};
};

} // namespace lapis::supervisor

#endif
