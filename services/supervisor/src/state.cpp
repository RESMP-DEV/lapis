#include "lapis/supervisor/state.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <utility>

namespace lapis::supervisor {
namespace {

bool hex_digit(char value) {
    const auto byte = static_cast<unsigned char>(value);
    return (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f');
}

bool bounded_text(const std::string& value, std::size_t maximum, bool allow_empty) {
    if ((!allow_empty && value.empty()) || value.size() > maximum ||
        std::any_of(value.begin(), value.end(), [](char value) {
            const auto byte = static_cast<unsigned char>(value);
            return byte == 0 || byte < 0x20 || byte == 0x7f;
        })) {
        return false;
    }
    return true;
}

void check(bool valid, const char* message) {
    if (!valid)
        throw std::runtime_error(message);
}

} // namespace

bool valid_hex(const std::string& value, std::size_t length) {
    return value.size() == length && std::all_of(value.begin(), value.end(), hex_digit);
}

bool valid_desired_session(const DesiredSession& session) {
    return bounded_text(session.endpoint, max_endpoint_bytes, false) &&
           session.endpoint.starts_with('/') &&
           valid_hex(session.fingerprint, fingerprint_hex_bytes) &&
           valid_hex(session.identity.session_id, identity_hex_bytes) &&
           valid_hex(session.identity.epoch, identity_hex_bytes) &&
           bounded_text(session.blocked_reason, max_blocked_reason_bytes, true) &&
           (session.spawn_token.empty() || valid_hex(session.spawn_token, spawn_token_hex_bytes)) &&
           (session.desired_state == DesiredState::stopped || session.blocked_reason.empty());
}

bool valid_state(const SupervisorState& state) {
    if (state.version != 1 || !valid_hex(state.instance_epoch, epoch_hex_bytes))
        return false;
    if (!state.session)
        return !state.enabled;
    if (!valid_desired_session(*state.session))
        return false;
    return state.session->desired_state != DesiredState::started ||
           (state.enabled && !state.session->spawn_token.empty());
}

bool constant_time_equal(const std::string& trusted, const std::string& supplied) {
    // Compare a fixed window so a malformed caller cannot learn token length
    // or prefix from timing. Tokens are bounded before entering this call.
    constexpr std::size_t window = spawn_token_hex_bytes;
    unsigned diff = static_cast<unsigned>(trusted.size() ^ supplied.size());
    for (std::size_t index = 0; index < window; ++index) {
        const auto trusted_byte =
            index < trusted.size() ? static_cast<unsigned char>(trusted[index]) : 0U;
        const auto supplied_byte =
            index < supplied.size() ? static_cast<unsigned char>(supplied[index]) : 0U;
        diff |= static_cast<unsigned>(trusted_byte ^ supplied_byte);
    }
    return diff == 0;
}

MemoryStateStorage::MemoryStateStorage(std::optional<std::string> bytes)
    : bytes_{std::move(bytes)} {}

void MemoryStateStorage::acquire() {
    check(!locked_, "Memory state registry is already locked");
    locked_ = true;
}

void MemoryStateStorage::release() { locked_ = false; }

std::optional<std::string> MemoryStateStorage::load() const {
    check(locked_, "Memory state registry is not locked");
    return bytes_;
}

void MemoryStateStorage::store(const std::string& bytes) {
    check(locked_, "Memory state registry is not locked");
    check(bytes.size() <= max_state_bytes, "State exceeds its persistence bound");
    // A memory implementation records the durability facts that the POSIX
    // adapter proves with file-system calls.
    bytes_ = bytes;
    ++write_count_;
    fsynced_ = true;
    owner_only_ = true;
}

SupervisorRegistry::SupervisorRegistry(std::shared_ptr<StateStorage> storage,
                                       std::shared_ptr<StateCodec> codec,
                                       std::shared_ptr<IdentityProvider> identities)
    : storage_{std::move(storage)}, codec_{std::move(codec)}, identities_{std::move(identities)} {
    check(storage_ && codec_ && identities_, "Supervisor dependencies are required");
    storage_->acquire();
    try {
        if (const auto saved = storage_->load())
            state_ = codec_->decode(*saved);
        else
            state_.instance_epoch = identities_->instance_epoch();
        check(valid_state(state_), "Stored supervisor state is invalid");
    } catch (...) {
        storage_->release();
        throw;
    }
}

SupervisorRegistry::~SupervisorRegistry() {
    if (storage_)
        storage_->release();
}

DesiredSession SupervisorRegistry::start_record(DesiredSession session) const {
    if (session.identity.session_id.empty())
        session.identity.session_id = identities_->session_id();
    if (session.identity.epoch.empty())
        session.identity.epoch = identities_->session_epoch();
    session.desired_state = DesiredState::started;
    session.spawn_token = identities_->spawn_token();
    session.blocked_reason.clear();
    return session;
}

SupervisorState SupervisorRegistry::start(DesiredSession session) {
    SupervisorState next = state_;
    session = start_record(std::move(session));
    check(valid_desired_session(session), "Invalid desired session");
    next.enabled = true;
    next.session = std::move(session);
    commit(std::move(next));
    return state_;
}

SupervisorState SupervisorRegistry::stop() {
    check(state_.session.has_value(), "No desired session to stop");
    SupervisorState next = state_;
    next.session->desired_state = DesiredState::stopped;
    next.session->blocked_reason.clear();
    next.session->spawn_token = identities_->spawn_token();
    commit(std::move(next));
    return state_;
}

SupervisorState SupervisorRegistry::disable() {
    SupervisorState next = state_;
    next.enabled = false;
    if (next.session) {
        next.session->desired_state = DesiredState::stopped;
        next.session->spawn_token.clear();
        next.session->blocked_reason = "supervisor disabled";
    }
    commit(std::move(next));
    return state_;
}

ControlOutcome SupervisorRegistry::control(const ControlRequest& request) {
    if (request.instance_epoch != state_.instance_epoch)
        return {ControlStatus::stale_epoch, state_};
    // A fresh registry has no token, so it admits no authenticated control
    // request. Local authoritative initialization must create the first slot.
    static constexpr std::array<unsigned char, spawn_token_hex_bytes> dummy{};
    const auto trusted = state_.session && !state_.session->spawn_token.empty()
                             ? state_.session->spawn_token
                             : std::string(dummy.size(), '0');
    if (!constant_time_equal(trusted, request.token))
        return {ControlStatus::unauthorized, state_};
    switch (request.action) {
    case ControlAction::start:
        if (!request.session)
            return {ControlStatus::rejected, state_};
        return {ControlStatus::applied, start(*request.session)};
    case ControlAction::stop:
        if (!state_.session)
            return {ControlStatus::rejected, state_};
        return {ControlStatus::applied, stop()};
    case ControlAction::disable:
        return {ControlStatus::applied, disable()};
    }
    throw std::runtime_error("Unknown supervisor control action");
}

void SupervisorRegistry::commit(SupervisorState next) {
    check(valid_state(next), "Supervisor transition produced invalid state");
    const auto bytes = codec_->encode(next);
    check(bytes.size() <= max_state_bytes, "Supervisor state exceeds its persistence bound");
    storage_->store(bytes);
    state_ = std::move(next);
}

} // namespace lapis::supervisor
