#ifndef LAPIS_SUPERVISOR_CONTROL_SERVER_HPP
#define LAPIS_SUPERVISOR_CONTROL_SERVER_HPP

#include "lapis/supervisor/runtime.hpp"

#include <functional>
#include <memory>
#include <string>

namespace lapis::supervisor {

using ControlHandler = std::function<std::string(uid_t peer_uid, const std::string& request)>;

class LocalControlServer final {
  public:
    LocalControlServer(std::string endpoint, ControlHandler handler);
    ~LocalControlServer();
    LocalControlServer(const LocalControlServer&) = delete;
    LocalControlServer& operator=(const LocalControlServer&) = delete;

    // Handle at least one ready event, or return after timeout_ms. This bounds
    // each client to one strict control frame and one reply.
    void poll_once(int timeout_ms);

  private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace lapis::supervisor

#endif
