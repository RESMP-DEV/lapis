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
} // namespace lapis::desktop
#endif
