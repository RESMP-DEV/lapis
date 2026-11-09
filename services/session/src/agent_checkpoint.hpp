#ifndef LAPIS_AGENT_CHECKPOINT_HPP
#define LAPIS_AGENT_CHECKPOINT_HPP
#include <QByteArray>
#include <QByteArrayView>
#include <QString>
#include <cstdint>
#include <optional>

#include "launch_spec.hpp"

namespace lapis::session {
// How a conversation's identity was learned: printed by the agent's session
// hook (terminal), by lapis's own Claude observer or Codex notify program, or
// before lapis recorded which (legacy, a version 1 record). Printed output
// never replaces an identity the observer learned.
enum class ResumeSource : std::uint8_t { terminal, observer, legacy };

struct ResumeRecord {
    QString agent;      // CLI name, for example "claude"
    QString session_id; // the conversation its native resume option takes
    ResumeSource source{ResumeSource::terminal};
};

// A conversation identifier safe to pass as one literal argument: printable,
// no whitespace, and never an option.
[[nodiscard]] bool valid_resume_identity(const QString& value);

// Checkpoints are untrusted terminal output. Tie their claimed agent to the
// managed Codex/Claude integration or the terminal executable's harness name.
[[nodiscard]] QString checkpoint_agent_for_launch(const LaunchSpec& launch);

// Whether this harness has a lapis conversation observer. This is a resume
// policy capability, so it remains true for an older service launched before
// an observer was attached.
[[nodiscard]] bool observer_backed_agent(const QString& agent);

// Agent session hooks for terminal restore tools write
// OSC 1337 SetUserVar=agent_checkpoint=<base64 JSON> to their terminal. This
// recognizes it in PTY output across read boundaries, keeping a bounded tail.
class CheckpointScanner {
  public:
    // The newest valid local checkpoint completed by this output, if any.
    [[nodiscard]] std::optional<ResumeRecord> scan(QByteArrayView output);

  private:
    QByteArray carry_;
};

// <endpoint>.resume: owner-only JSON beside the private endpoint, written when
// the agent reports its conversation. Reading returns nullopt for absent,
// unsafe or malformed files; writing replaces the file atomically and throws
// on failure.
[[nodiscard]] std::optional<ResumeRecord> read_resume_record(const QString& endpoint);

// Whether a Codex thread is a conversation of its own rather than a subagent's,
// from its rollout under <codex_home>/sessions/YYYY/MM/DD (the first line says
// which), or nullopt when the id is not a thread id or no readable rollout
// names it.
[[nodiscard]] std::optional<bool> codex_main_thread(const QString& codex_home,
                                                    const QString& thread);
void write_resume_record(const QString& endpoint, const ResumeRecord& record);
} // namespace lapis::session
#endif
