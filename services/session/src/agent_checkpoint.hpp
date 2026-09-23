#ifndef LAPIS_AGENT_CHECKPOINT_HPP
#define LAPIS_AGENT_CHECKPOINT_HPP
#include <QByteArray>
#include <QByteArrayView>
#include <QString>
#include <optional>

namespace lapis::session {
// The conversation a service-owned agent can resume after its service stops.
struct ResumeRecord {
    QString agent;      // CLI name, for example "claude"
    QString session_id; // the conversation its native resume option takes
};

// A conversation identifier safe to pass as one literal argument: printable,
// no whitespace, and never an option.
[[nodiscard]] bool valid_resume_identity(const QString& value);

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
void write_resume_record(const QString& endpoint, const ResumeRecord& record);
} // namespace lapis::session
#endif
