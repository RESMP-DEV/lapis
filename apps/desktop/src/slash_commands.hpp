#ifndef LAPIS_DESKTOP_SLASH_COMMANDS_HPP
#define LAPIS_DESKTOP_SLASH_COMMANDS_HPP
#include <QString>
#include <QVariantList>

namespace lapis::desktop {
// The slash commands a Claude Code agent takes, for the visual view's bar:
// the CLI's own (as Claude Code 2.1.319 lists them, without hidden and
// internal ones), then the person's commands and skills from
// ~/.claude/{commands,skills} and the agent's folder's .claude/, each as
// {name, description, source}. `home` and `folder` may be empty.
[[nodiscard]] QVariantList claude_slash_commands(const QString& home, const QString& folder);

// Codex CLI's own slash commands (Codex 0.157.1 has no custom commands to
// read: its prompts directory is gone and skills are `$name` mentions).
// Each is {name, description, source, terminal}; `terminal` marks one whose
// picker, panel or output shows only in the terminal, which the thread
// cannot draw for Codex.
[[nodiscard]] QVariantList codex_slash_commands(const QString& home, const QString& folder);

// Antigravity CLI's (agy) own slash commands, then the skills it offers as
// commands: the agent folder's .agents/skills (also .agent, _agents,
// _agent, from the folder up to its repository root), then the person's
// ~/.gemini/config/skills, without those marked `disable-slash-command`.
// Entries are shaped as codex_slash_commands' are.
[[nodiscard]] QVariantList agy_slash_commands(const QString& home, const QString& folder);

// The list for an agent's harness id ("claude", "codex", "agy"); empty for
// a CLI whose commands lapis does not know.
[[nodiscard]] QVariantList slash_commands(const QString& harness, const QString& home,
                                          const QString& folder);
} // namespace lapis::desktop
#endif
