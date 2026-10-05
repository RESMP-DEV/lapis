#ifndef LAPIS_SUPERVISOR_RUNTIME_HPP
#define LAPIS_SUPERVISOR_RUNTIME_HPP

#include "lapis/supervisor/state.hpp"

#include <sys/types.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace lapis::supervisor {

inline constexpr unsigned max_control_clients = 4;
inline constexpr unsigned max_restarts_per_window = 3;
inline constexpr std::uint64_t restart_window_ns = std::uint64_t{60} * 1'000'000'000;

enum class ControlReplyStatus : std::uint8_t {
    applied = 0,
    unauthorized = 1,
    stale_epoch = 2,
    rejected = 3
};

enum class Convergence : std::uint8_t {
    converged = 0,
    started = 1,
    restarted = 2,
    stopped = 3,
    disabled = 4,
    exhausted = 5,
    failed = 6
};

struct ChildProcess {
    pid_t pid{-1};
    std::string spawn_token;
    std::string endpoint;
    int transport{-1};
    bool adopted{false};
    // The real session launcher records the protocol identity it verified.
    // A generic harmless-child launcher may leave these empty; its process is
    // then terminated by PID only instead of signalling a process group.
    std::string session_id;
    std::string session_epoch;
    std::string fingerprint;
};

// The runtime owns lifecycle policy only. A production adapter still leaves
// PTY, terminal and protocol ownership in the existing session-service child.
class ChildLauncher {
  public:
    virtual ~ChildLauncher() = default;
    ChildLauncher(const ChildLauncher&) = delete;
    ChildLauncher& operator=(const ChildLauncher&) = delete;

    // Return a live child carrying this exact spawn token, or nullopt. This is
    // the reconnect seam: a restarted supervisor must adopt before launching.
    [[nodiscard]] virtual std::optional<ChildProcess> adopt(const DesiredSession& session,
                                                            const std::string& spawn_token) = 0;
    [[nodiscard]] virtual ChildProcess launch(const DesiredSession& session,
                                              const std::string& spawn_token) = 0;
    [[nodiscard]] virtual bool alive(const ChildProcess& child) = 0;
    [[nodiscard]] virtual bool terminate(const ChildProcess& child) = 0;
    virtual void release(const ChildProcess& child) = 0;

  protected:
    ChildLauncher() = default;
};

struct LaunchCommand {
    std::string program;
    std::vector<std::string> arguments;
};

// Experimental process launcher. The child is expected to be cooperative and
// harmless unless it is the existing session service qualified separately.
class OwnershipChildLauncher final : public ChildLauncher {
  public:
    explicit OwnershipChildLauncher(LaunchCommand command);

    [[nodiscard]] std::optional<ChildProcess> adopt(const DesiredSession& session,
                                                    const std::string& spawn_token) override;
    [[nodiscard]] ChildProcess launch(const DesiredSession& session,
                                      const std::string& spawn_token) override;
    [[nodiscard]] bool alive(const ChildProcess& child) override;
    [[nodiscard]] bool terminate(const ChildProcess& child) override;
    void release(const ChildProcess& child) override;

  private:
    LaunchCommand command_;
};

class MonotonicClock {
  public:
    virtual ~MonotonicClock() = default;
    MonotonicClock(const MonotonicClock&) = delete;
    MonotonicClock& operator=(const MonotonicClock&) = delete;
    [[nodiscard]] virtual std::uint64_t nanoseconds() = 0;

  protected:
    MonotonicClock() = default;
};

class SupervisorRuntime final {
  public:
    SupervisorRuntime(std::shared_ptr<SupervisorRegistry> registry,
                      std::shared_ptr<ChildLauncher> launcher,
                      std::shared_ptr<MonotonicClock> clock = nullptr);

    // Local authorization bootstrap. Remote control still requires a token and
    // the registry deliberately refuses control before the first slot exists.
    [[nodiscard]] SupervisorState bootstrap(DesiredSession session);
    [[nodiscard]] std::string control(uid_t peer_uid, const std::string& request);
    [[nodiscard]] Convergence converge();
    [[nodiscard]] const SupervisorState& state() const noexcept;
    [[nodiscard]] pid_t child_pid() const noexcept;

  private:
    [[nodiscard]] Convergence launch_desired();
    void retire_child();
    bool stop_desired(const std::string& reason);

    std::shared_ptr<SupervisorRegistry> registry_;
    std::shared_ptr<ChildLauncher> launcher_;
    std::shared_ptr<MonotonicClock> clock_;
    std::optional<ChildProcess> child_;
    bool initial_admission_used_{false};
    bool startup_convergence_failed_{false};
    std::vector<std::uint64_t> restart_admissions_;
};

[[nodiscard]] std::string encode_control_reply(ControlReplyStatus status);
[[nodiscard]] ControlReplyStatus control_reply_status(ControlStatus status);

} // namespace lapis::supervisor

#endif
