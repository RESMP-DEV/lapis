#ifndef LAPIS_SUPERVISOR_CLIENT_HPP
#define LAPIS_SUPERVISOR_CLIENT_HPP

#include "lapis/supervisor/state.hpp"

#include <cstdint>
#include <string>

namespace lapis::supervisor {

inline constexpr int supervisor_client_timeout_ms = 5000;

struct SupervisorStatus {
    int schema_version{};
    bool enabled{};
    bool service_live{};
    bool reconstructable{};
    std::string instance_epoch;
    std::string slot;
    std::string blocked_reason;
    std::string endpoint;
};

[[nodiscard]] std::string encode_status_request();
[[nodiscard]] std::string encode_status_reply(const SupervisorState& state, bool service_live);
[[nodiscard]] std::string encode_stop_request(const SupervisorState& state);
[[nodiscard]] std::string encode_reset_request(const SupervisorState& state);
[[nodiscard]] SupervisorStatus decode_status_reply(const std::string& bytes);
[[nodiscard]] SupervisorState read_control_state(const std::string& directory);
[[nodiscard]] std::string send_control_request(const std::string& endpoint,
                                               const std::string& request);

} // namespace lapis::supervisor

#endif
