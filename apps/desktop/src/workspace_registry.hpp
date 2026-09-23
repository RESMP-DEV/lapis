#ifndef LAPIS_DESKTOP_WORKSPACE_REGISTRY_HPP
#define LAPIS_DESKTOP_WORKSPACE_REGISTRY_HPP

#include "launch_spec.hpp"
#include "transport/local_protocol.hpp"
#include <memory>
#include <vector>

namespace lapis::desktop {
// Manifest v1 stores reconnect metadata only, never launch arguments or secrets.
struct WorkspaceEntry {
    QString endpoint;
    session::wire::SessionIdentity identity;
    QByteArray fingerprint;
    QString title;
    QString directory;
    session::AgentMode agent{session::AgentMode::terminal};
    bool operator==(const WorkspaceEntry&) const = default;
};

// Blocking filesystem operations: call from a serialized workspace I/O worker.
// Lifetime holds the exclusive workspace writer lock. No process ownership.
class WorkspaceRegistry final {
  public:
    static constexpr std::size_t maximum_entries = 8;
    explicit WorkspaceRegistry(const QString& path);
    ~WorkspaceRegistry();
    WorkspaceRegistry(const WorkspaceRegistry&) = delete;
    WorkspaceRegistry& operator=(const WorkspaceRegistry&) = delete;
    [[nodiscard]] std::vector<WorkspaceEntry> read() const;
    void write(const std::vector<WorkspaceEntry>& entries);

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace lapis::desktop
#endif
