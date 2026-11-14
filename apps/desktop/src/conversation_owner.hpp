#ifndef LAPIS_DESKTOP_CONVERSATION_OWNER_HPP
#define LAPIS_DESKTOP_CONVERSATION_OWNER_HPP
#include <QHash>
#include <QList>
#include <QString>

// Which conversation a local Codex or Antigravity (agy) agent is writing when
// its CLI has not named it to lapis: agy never does, and Codex only when its
// first turn ends. Each answer is lapis's pin for that agent; the workspace
// keeps it in the agent's registry record.
//
// The first source is the agent's own processes: its session service's pid
// is in `<endpoint>.lock`, and the CLI below it holds its conversation open
// (agy 1.5 `presence/<id>.lock` and `conversations/<id>.db`; Codex its
// rollout). That follows a new conversation (`/clear`) in the same process,
// and no other agent's process can hold it. Only before the CLI has a
// conversation open, or when its processes cannot be read, does the start
// time decide: the earliest conversation begun in the agent's folder since its
// service started that no other agent has, and only when no other running,
// still-unresolved agent of that CLI in that folder could have begun it.
namespace lapis::desktop::conversation_owner {
// The CLIs' homes: $CODEX_HOME or ~/.codex, and ~/.gemini/antigravity-cli.
[[nodiscard]] QString codex_home();
[[nodiscard]] QString antigravity_home();
// Where agy 1.5 appends a conversation's steps, or empty before it has.
[[nodiscard]] QString antigravity_transcript(const QString& conversation);

struct Agent {
    QString id;
    QString harness; // "codex" or "agy"; any other is left alone
    QString folder;
    QString endpoint; // its session service's; made when the service starts
    // The conversation lapis knows: the CLI's own record, else the pin, else
    // the one lapis resumed. Empty when none.
    QString known;
    // `known` is the CLI's own record (notify, hook or observer), which the
    // workspace prefers; such an agent is never pinned.
    bool recorded{};
};

// The pins to change: agent id to conversation id, for agents whose
// conversation is newly found or moved to another (`/clear`).
[[nodiscard]] QHash<QString, QString> pins(const QList<Agent>& agents);

// The conversation the processes of the service at `endpoint` hold open for
// `harness`, or empty. When they hold several, `pin` if it is one of them,
// else the newest.
[[nodiscard]] QString held_conversation(const QString& harness, const QString& endpoint,
                                        const QString& pin);

// The pid of the session service at `endpoint`, from the lock it holds, when
// that service is running on this machine; else 0.
[[nodiscard]] qint64 service_pid(const QString& endpoint);
} // namespace lapis::desktop::conversation_owner
#endif
