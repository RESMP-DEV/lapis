#ifndef LAPIS_DESKTOP_FACTORY_STATE_HPP
#define LAPIS_DESKTOP_FACTORY_STATE_HPP

#include <QByteArray>
#include <QHash>
#include <QString>
#include <QStringList>

#include <cstdint>

// Factory view: every open worker sorted into four pipeline stages. A
// separate judge model may write <registry folder>/factory_state.json with
// its verdict per agent; live agent state always takes precedence over a
// verdict older than the agent's last change, and agents the judge has not
// seen get a deterministic stage from their live state alone.
//
//   {"version": 1, "updatedAtMs": ..., "judgeModel": "...",
//    "agents": {"<id>": {"stage": "needs_you|in_progress|landed|idle_cull",
//                        "keepNeeded": false, "headline": "...", "reason": "...",
//                        "receipt": "...", "judgedAtMs": ...}}}
namespace lapis::desktop::factory {

inline constexpr const char* kNeedsYou = "needs_you";
inline constexpr const char* kInProgress = "in_progress";
inline constexpr const char* kLanded = "landed";
inline constexpr const char* kIdleCull = "idle_cull";
// The four stages in board order.
[[nodiscard]] QStringList stages();
[[nodiscard]] bool known_stage(const QString& stage);

// A finished or ready agent counts as landed for this long after its turn
// ended; after that it is idle and the judge decides whether it is needed.
inline constexpr qint64 kLandedRecentMs = qint64{6} * 60 * 60 * 1000;
// factory_state.json is read only up to this size.
inline constexpr qint64 kMaxStateBytes = qint64{4} * 1024 * 1024;

struct Verdict {
    QString stage;
    bool keep_needed{true};
    QString headline;
    QString reason;
    QString receipt;
    qint64 judged_at_ms{};
};

struct State {
    bool loaded{}; // a readable file was found
    qint64 updated_at_ms{};
    QString judge_model;
    QHash<QString, Verdict> agents;
};

// Never throws: a malformed file or entry is skipped, a missing field is empty.
[[nodiscard]] State parse(const QByteArray& json);
// Empty (not loaded) when the file is missing, a symlink, unreadable or too large.
[[nodiscard]] State read(const QString& path);

// What lapis knows about an agent now.
struct Live {
    QString status_kind;      // Agent::statusKind()
    bool attention_pending{}; // a request waits for the person
    qint64 changed_at_ms{};   // its status or requests last changed; 0 unknown
    qint64 finished_at_ms{};  // its last turn ended; 0 unknown
    qint64 now_ms{};
};

// The stage from live state alone.
[[nodiscard]] QString fallback_stage(const Live& live);
// The stage shown: a pending request or waiting agent needs you and a running
// turn is in progress whatever the verdict says; otherwise a settled verdict
// holds until the agent changes after it was judged (an in_progress verdict
// on a turn that is no longer running is stale); otherwise the fallback.
[[nodiscard]] QString stage(const Live& live, const Verdict* verdict);
// Whether a verdict still describes the agent (it has not changed since).
[[nodiscard]] bool verdict_current(const Live& live, const Verdict& verdict);

} // namespace lapis::desktop::factory
#endif // LAPIS_DESKTOP_FACTORY_STATE_HPP
