#ifndef LAPIS_SUPERVISOR_SESSION_SERVICE_LAUNCHER_HPP
#define LAPIS_SUPERVISOR_SESSION_SERVICE_LAUNCHER_HPP

#include "lapis/supervisor/runtime.hpp"

#include <QString>
#include <QStringList>

#include <memory>
#include <string>

namespace lapis::session {
struct LaunchSpec;
}

namespace lapis::supervisor {

// The launch description is fixed at probe construction, not persisted by this
// first slice. A persistent daemon must version and persist the launch payload
// before it can reconstruct state outside its own process.
struct SessionServiceLaunch {
    QString service_program;
    QString agent_program;
    QStringList agent_arguments;
    QString working_directory;
    unsigned columns{80};
    unsigned rows{24};
    bool codex{false};
    bool claude{false};
};

class SessionServiceLauncher final : public ChildLauncher {
  public:
    SessionServiceLauncher(SessionServiceLaunch launch, std::string fingerprint);

    [[nodiscard]] std::optional<ChildProcess> adopt(const DesiredSession& session,
                                                    const std::string& spawn_token) override;
    [[nodiscard]] bool peer_preserved(const DesiredSession& session,
                                      const std::string& spawn_token) override;
    [[nodiscard]] ChildProcess launch(const DesiredSession& session,
                                      const std::string& spawn_token) override;
    [[nodiscard]] bool alive(const ChildProcess& child) override;
    [[nodiscard]] bool terminate(const ChildProcess& child) override;
    void release(const ChildProcess& child) override;

  private:
    [[nodiscard]] QStringList service_arguments(const DesiredSession& session) const;
    [[nodiscard]] bool handshake(const DesiredSession& session, pid_t child_pid);

    SessionServiceLaunch launch_;
    std::string fingerprint_;
};

} // namespace lapis::supervisor

#endif
