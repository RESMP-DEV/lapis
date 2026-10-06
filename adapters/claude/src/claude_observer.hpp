#ifndef LAPIS_CLAUDE_OBSERVER_HPP
#define LAPIS_CLAUDE_OBSERVER_HPP
#include <QJsonObject>
#include <QObject>
#include <QStringList>
#include <cstdint>
#include <lapis/session/attention.hpp>
#include <memory>

namespace lapis::claude {
// Service-thread owner of a private, observation-only hook channel.
class Observer final : public QObject {
    Q_OBJECT
  public:
    // Where hooks arrive: the private local socket this observer listens on,
    // or frames another machine's relay printed to the agent's terminal,
    // which the session service authenticates and passes to receiveRelayed.
    enum class Transport : std::uint8_t { local_socket, terminal };
    explicit Observer(session::attention::State& state, QObject* parent = nullptr);
    Observer(session::attention::State& state, Transport transport, QObject* parent = nullptr);
    ~Observer() override;
    [[nodiscard]] const QString& diagnostic() const;
    // The Claude Code session the hooks are bound to; empty before SessionStart
    // and after /clear until the next session starts.
    [[nodiscard]] const QString& sessionId() const;
    [[nodiscard]] QJsonObject details(const session::attention::RequestId& id) const;
    [[nodiscard]] QStringList launchArguments(const QStringList& original,
                                              const QString& serviceExecutable);
    // One authenticated hook input from a terminal relay: the hook's own JSON
    // reduced to the relay's identity fields and background-work lists.
    // Local-socket observers never need it.
    void receiveRelayed(const QJsonObject& source);
    void stop();
    // How long a turn that ended with background work in flight waits for
    // that work's next turn before it counts as finished.
    static constexpr int paused_turn_ms = 10 * 60 * 1000;
    void setPausedTurnMsForTesting(int ms);
  signals:
    // State changes immediately; notifications are coalesced on the event loop.
    // Receivers may stop or destroy the observer without reentering transport work.
    void changed();

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace lapis::claude
#endif
