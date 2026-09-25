#ifndef LAPIS_AGENT_CHECKPOINT_HPP
#define LAPIS_AGENT_CHECKPOINT_HPP
#include <QByteArray>
#include <QByteArrayView>
#include <QString>
#include <cstdint>
#include <optional>
#include <vector>

#include "launch_spec.hpp"

namespace lapis::session {
enum class ResumeSource : std::uint8_t { terminal, observer };

// A terminal checkpoint is advisory; only an observer can authorize automatic resume.
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

// The Codex threads whose rollouts a Codex app-server holds open, from its
// `lsof -Fn` listing, most recently written first. Codex keeps every loaded
// thread's rollout open, including the previous conversation after /new or
// /resume; subagent threads, which say so in their rollout's first line, are
// left out. Rollouts that are not readable regular files are skipped.
[[nodiscard]] std::vector<QString> codex_threads_from_open_files(const QString& listing);
// The conversation in use: the first of codex_threads_from_open_files.
[[nodiscard]] std::optional<QString> codex_thread_from_open_files(const QString& listing);
void write_resume_record(const QString& endpoint, const ResumeRecord& record);
} // namespace lapis::session
#endif
