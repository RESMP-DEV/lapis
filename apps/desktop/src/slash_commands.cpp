#include "slash_commands.hpp"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QSet>
#include <QVariantMap>
#include <cstddef>

namespace lapis::desktop {
namespace {
// Claude Code 2.1.319's commands with their own descriptions, read from its
// command table; tools, hidden and plan-gated entries are left out.
struct Builtin {
    const char* name;
    const char* description;
};
constexpr Builtin claude_builtins[] = {
    {"add-dir", "Add a new working directory"},
    {"advisor", "Let Claude consult a stronger model at key moments"},
    {"alias", "Create or list command aliases"},
    {"autocompact", "Set how full the context gets before auto-summarizing"},
    {"background", "Send this session to the background and free the terminal"},
    {"batch", "Plan a large change, then run it in parallel across worktree agents"},
    {"branch", "Create a branch of the current conversation at this point"},
    {"brief", "Toggle brief-only mode"},
    {"btw", "Ask a quick side question without interrupting the main conversation"},
    {"cd", "Move this session to a new working directory"},
    {"clear", "Start a new session with empty context; previous session stays on disk"},
    {"code-review", "Review the current diff or a PR for correctness bugs"},
    {"color", "Set the prompt bar color for this session"},
    {"compact", "Free up context by summarizing the conversation so far"},
    {"config", "Open settings"},
    {"context", "Visualize current context usage as a colored grid"},
    {"copy", "Copy Claude's last response to clipboard (or /copy N for the Nth-latest)"},
    {"daemon", "Manage background services and routines"},
    {"debug", "Enable debug logging for this session and help diagnose issues"},
    {"effort", "Set effort level for model usage"},
    {"export", "Export the current conversation to a file or clipboard"},
    {"fast", "Toggle fast mode"},
    {"feedback", "Send feedback to Anthropic or report a bug"},
    {"focus", "Toggle focus view: just your prompt, summary, and response"},
    {"fork", "Copy this conversation into a new background session and keep working here"},
    {"goal", "Set a goal Claude checks before stopping"},
    {"help", "Show help and available commands"},
    {"hooks", "View hook configurations for tool events"},
    {"ide", "Manage IDE integrations and show status"},
    {"import", "Import config from another AI coding agent"},
    {"init", "Initialize a new CLAUDE.md file with codebase documentation"},
    {"insights", "Generate a report analyzing your Claude Code sessions"},
    {"list-agents", "List subagents, teammates, and other Claude sessions you can message"},
    {"login", "Sign in with your Anthropic account"},
    {"logout", "Sign out from your Anthropic account"},
    {"loop", "Run a prompt or slash command on a recurring interval"},
    {"loops", "List, create, and delete loops"},
    {"mcp", "Manage MCP servers"},
    {"memory", "Edit CLAUDE.md files and memory settings"},
    {"model", "Set the AI model for Claude Code"},
    {"output-style", "List output styles or switch to one"},
    {"permissions", "Manage allow and deny tool permission rules"},
    {"plan", "Enable plan mode or view the current session plan"},
    {"plugin", "Manage Claude Code plugins"},
    {"recap", "Generate a one-line session recap now"},
    {"reload-plugins", "Activate pending plugin changes in the current session"},
    {"reload-skills", "Pick up skills added or changed on disk during this session"},
    {"rename", "Rename the current conversation"},
    {"resume", "Resume a previous conversation"},
    {"schedule", "Create and manage scheduled remote Claude Code agents"},
    {"security-review", "Complete a security review of the pending changes"},
    {"simplify", "Review changed code for reuse and simplification, then apply fixes"},
    {"skills", "List available skills"},
    {"status", "Show Claude Code status including version, model, account and tools"},
    {"subtask", "Send a subagent off with your full context; its result comes back here"},
    {"teleport", "Send this session to the cloud, or resume one from claude.ai"},
    {"theme", "Change the theme"},
    {"tui", "Set the terminal UI renderer (default | fullscreen)"},
    {"usage", "Show session cost, plan usage, and activity stats"},
    {"version", "Show this session's version"},
    {"voice", "Toggle voice mode"},
    {"workflows", "Browse running and completed workflows"},
};

// A command of a CLI whose terminal dialogs the visual view cannot see:
// `terminal` marks one whose picker, panel or output shows only there.
struct TerminalBuiltin {
    const char* name;
    const char* description;
    bool terminal;
};

// Codex CLI 0.157.1's commands in its popup's order, with its own
// descriptions: codex-rs/tui/src/slash_command.rs at tag rust-v0.157.1
// (github.com/openai/codex), checked on 2026-11-11 against the installed
// binary's popup under a throwaway terminal. Left out as the popup leaves
// them out: aliases (/quit, /btw), /apps, /debug-*, /setup-default-sandbox
// (Windows only), /rollout and /test-approval (debug builds). Left out as
// unsafe to send blind: /exit, /archive and /delete, which end or delete the
// session. /fast is the model's service tier command, shown after /model
// for models that offer it. Only /init and /compact (a turn of their own)
// and quiet toggles stay in the thread; the rest answer in the terminal.
constexpr TerminalBuiltin codex_builtins[] = {
    {"model", "choose what model and reasoning effort to use", true},
    {"fast", "2x speed, increased usage", false},
    {"ide", "include current selection, open files, and other context from your IDE", true},
    {"permissions", "choose what Codex is allowed to do", true},
    {"keymap", "remap TUI shortcuts", true},
    {"vim", "toggle Vim mode for the composer", false},
    {"experimental", "toggle experimental features", true},
    {"approve", "approve one retry of a recent auto-review denial", true},
    {"memories", "configure memory use and generation", true},
    {"skills", "use skills to improve how Codex performs specific tasks", true},
    {"import", "import setup, this project, and recent chats from Claude Code", true},
    {"hooks", "view and manage lifecycle hooks", true},
    {"review", "review my current changes and find issues", true},
    {"rename", "rename the current thread", true},
    {"new", "start a new chat during a conversation", true},
    {"resume", "resume a saved chat", true},
    {"fork", "fork the current chat", true},
    {"worktree", "start or continue a conversation in a new worktree", true},
    {"app", "continue this session in the Desktop app", true},
    {"init", "create an AGENTS.md file with instructions for Codex", false},
    {"compact", "summarize conversation to prevent hitting the context limit", false},
    {"recap", "summarize the current conversation now", true},
    {"plan", "switch to Plan mode", true},
    {"voice", "start or stop voice; use /voice settings to choose a voice", true},
    {"goal", "set or view the goal for a long-running task", true},
    {"agents", "open the agent command center", true},
    {"side", "start a side conversation in an ephemeral fork", true},
    {"copy", "copy the last response or part of it", false},
    {"export", "export the conversation as markdown", true},
    {"raw", "toggle raw scrollback mode for copy-friendly terminal selection", true},
    {"tui", "choose the TUI mode for the next launch", true},
    {"diff", "show git diff (including untracked files)", true},
    {"mention", "mention a file", true},
    {"status", "show current session configuration and token usage", true},
    {"daemon", "Manage the local background server", true},
    {"warnings", "view retained warnings and diagnostic details", true},
    {"cd", "change the current working directory", true},
    {"pwd", "show the current working directory", true},
    {"usage", "view account usage or use a usage limit reset", true},
    {"title", "configure which items appear in the terminal title", true},
    {"statusline", "configure which items appear in the status line", true},
    {"theme", "choose a syntax highlighting theme", true},
    {"pets", "choose or hide the terminal pet", true},
    {"mcp", "list configured MCP tools; use /mcp verbose for details", true},
    {"plugins", "browse plugins", true},
    {"logout", "log out of Codex", true},
    {"feedback", "send logs to maintainers", true},
    {"ps", "list background terminals", true},
    {"stop", "stop all background terminals", true},
    {"clear", "clear the terminal and start a new chat", true},
    {"subagents", "switch between this session's subagents", true},
};

// Antigravity CLI 1.5.6's commands in its popup's order, with its own
// descriptions, read from the installed binary's popup under a throwaway
// terminal on 2026-11-11 (the public list is
// antigravity.google/docs/cli/reference). The last three are skills agy
// ships and mounts itself. Left out as unsafe to send blind: /exit, which
// ends the session. Commands that run a turn (the planning and agent ones,
// /compact, the shipped skills) and quiet toggles stay in the thread; the
// rest open a panel or picker in the terminal.
constexpr TerminalBuiltin agy_builtins[] = {
    {"add-dir", "Add a directory to the workspace", true},
    {"agents", "List available custom agents", true},
    {"artifact", "View and review artifacts", true},
    {"btw", "Ask a side question without interrupting the current task", true},
    {"changelog", "Show release notes and changes", true},
    {"clear", "Clear conversation and start a new one", true},
    {"codesearch", "Search code in the workspace (usage: /codesearch <query>)", true},
    {"config", "Open settings panel", true},
    {"context", "Visualize current context usage", true},
    {"copy", "Copy the last planner response to the clipboard, the n-th with '/copy n'", false},
    {"credits", "Show remaining G1 credits and purchase link", true},
    {"diff", "View uncommitted changes and per-turn diffs", true},
    {"effort", "Set the reasoning effort", true},
    {"feedback", "Submit qualitative feedback to improve the agent", true},
    {"fork", "Create a branch of the current conversation at this point", true},
    {"help", "Show available commands and keybindings", true},
    {"hooks", "Manage hook configurations for tool events", true},
    {"keybindings", "Set custom keybindings", true},
    {"logout", "Log out", true},
    {"mcp", "Manage MCP servers", true},
    {"model", "Set a model, or run a single prompt on another model", true},
    {"open", "Open a file or view opened/edited files", true},
    {"permissions", "Manage tool permissions", true},
    {"plugin", "Manage, discover, install, uninstall, enable, and disable plugins", true},
    {"projects", "Create and edit projects", true},
    {"record", "Dictate a prompt using your microphone", true},
    {"remote-control", "Turn on remote control for this session (off to disable)", true},
    {"rename", "Rename the current conversation", true},
    {"resume", "Browse and resume past conversations", true},
    {"rewind", "Rewind conversation to a previous message", true},
    {"skills", "List available skills", true},
    {"statusline", "Toggle the statusline", false},
    {"tasks", "View background tasks", true},
    {"title", "Toggle custom terminal window title", false},
    {"usage", "View model quota usage", true},
    {"voice", "Dictate a prompt or talk with the agent by voice", true},
    {"goal", "Run until the specified goal is completely finished.", false},
    {"compact", "Compact the conversation now (summarize history to free up the context window).",
     false},
    {"schedule", "Run an instruction on a recurring schedule or as a one-time timer.", false},
    {"browser", "Invoke a browser agent for web tasks.", false},
    {"plan", "Plan carefully before executing a task.", false},
    {"grill-me", "Interview me to align on a plan.", false},
    {"teamwork", "Invoke a team of agents to autonomously tackle large projects.", false},
    {"learn", "Reflect on recent successes or corrections to capture reusable skills or rules.",
     false},
    {"boost", "Invoke the Boost multi-agent orchestrator for complex tasks.", false},
    {"agy-customizations",
     "Comprehensive guide and reference for the Antigravity Customization System.", false},
    {"antigravity-guide",
     "Provides a comprehensive guide, quick reference, and sitemap for Google Antigravity (AGY)",
     false},
    {"swarm", "(Preview & Quota Heavy) Run a swarm: several agents working in parallel on one task",
     false},
};

// A Markdown file's front matter value for `key`, or empty. A folded or
// literal block (`key: >-`) is joined into one line.
QString front_matter(const QString& text, const QString& key) {
    if (!text.startsWith(QLatin1String("---")))
        return {};
    const auto end = text.indexOf(QLatin1String("\n---"), 3);
    const auto head = text.mid(3, end < 0 ? 0 : end - 3);
    const auto lines = head.split(QLatin1Char('\n'));
    for (qsizetype i = 0; i < lines.size(); ++i) {
        const auto trimmed = lines[i].trimmed();
        if (trimmed.startsWith(key + QLatin1Char(':'))) {
            auto value = trimmed.mid(key.size() + 1).trimmed();
            if (value == QLatin1String(">-") || value == QLatin1String(">") ||
                value == QLatin1String("|") || value == QLatin1String("|-")) {
                QStringList block;
                while (++i < lines.size() &&
                       (lines[i].startsWith(QLatin1Char(' ')) || lines[i].trimmed().isEmpty()))
                    block.append(lines[i].trimmed());
                return block.join(QLatin1Char(' ')).simplified();
            }
            if (value.size() >= 2 &&
                (value.front() == QLatin1Char('"') || value.front() == QLatin1Char('\'')) &&
                value.back() == value.front())
                value = value.mid(1, value.size() - 2);
            return value;
        }
    }
    return {};
}

// The description a command file gives: its front matter's, else its first
// line of text.
QString description_of(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    const auto text = QString::fromUtf8(file.read(8192));
    auto description = front_matter(text, QStringLiteral("description"));
    if (description.isEmpty()) {
        const auto body =
            text.startsWith(QLatin1String("---"))
                ? text.mid(std::max<qsizetype>(0, text.indexOf(QLatin1String("\n---"), 3) + 4))
                : text;
        for (const auto& line : body.split(QLatin1Char('\n')))
            if (!line.trimmed().isEmpty() && !line.trimmed().startsWith(QLatin1Char('#'))) {
                description = line.trimmed();
                break;
            }
    }
    return description.left(160);
}

void add_skills(QVariantList& out, QSet<QString>& seen, const QString& dir, const QString& source,
                bool marked);

void add_commands(QVariantList& out, QSet<QString>& seen, const QString& root,
                  const QString& source) {
    const QDir base(root + QStringLiteral("/commands"));
    if (base.exists()) {
        QDirIterator it(base.path(), {QStringLiteral("*.md")}, QDir::Files,
                        QDirIterator::Subdirectories);
        int count = 0;
        while (it.hasNext() && count < 400) {
            const auto path = it.next();
            auto name = base.relativeFilePath(path);
            name.chop(3);
            name.replace(QLatin1Char('/'), QLatin1Char(':'));
            if (seen.contains(name))
                continue;
            seen.insert(name);
            out.append(QVariantMap{{QStringLiteral("name"), name},
                                   {QStringLiteral("description"), description_of(path)},
                                   {QStringLiteral("source"), source}});
            ++count;
        }
    }
    add_skills(out, seen, root + QStringLiteral("/skills"), source, false);
}

// The skills in `dir` (<name>/SKILL.md), named by their front matter. With
// `marked`, each also carries terminal: false (a skill runs as a turn), and
// one marked `disable-slash-command: true` is left out, as agy does.
void add_skills(QVariantList& out, QSet<QString>& seen, const QString& dir, const QString& source,
                bool marked) {
    const QDir skills(dir);
    for (const auto& entry : skills.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        const auto path = skills.filePath(entry + QStringLiteral("/SKILL.md"));
        if (!QFile::exists(path))
            continue;
        QFile file(path);
        QString name = entry;
        if (file.open(QIODevice::ReadOnly)) {
            const auto head = QString::fromUtf8(file.read(4096));
            if (marked && front_matter(head, QStringLiteral("disable-slash-command")) ==
                              QLatin1String("true"))
                continue;
            const auto given = front_matter(head, QStringLiteral("name"));
            if (!given.isEmpty())
                name = given;
        }
        if (seen.contains(name))
            continue;
        seen.insert(name);
        QVariantMap item{{QStringLiteral("name"), name},
                         {QStringLiteral("description"), description_of(path)},
                         {QStringLiteral("source"), source}};
        if (marked)
            item.insert(QStringLiteral("terminal"), false);
        out.append(item);
    }
}

template <std::size_t N>
QVariantList terminal_builtins(const TerminalBuiltin (&builtins)[N], const QString& source,
                               QSet<QString>& seen) {
    QVariantList out;
    for (const auto& builtin : builtins) {
        const auto name = QString::fromLatin1(builtin.name);
        seen.insert(name);
        out.append(
            QVariantMap{{QStringLiteral("name"), name},
                        {QStringLiteral("description"), QString::fromUtf8(builtin.description)},
                        {QStringLiteral("source"), source},
                        {QStringLiteral("terminal"), builtin.terminal}});
    }
    return out;
}
} // namespace

QVariantList claude_slash_commands(const QString& home, const QString& folder) {
    QVariantList out;
    QSet<QString> seen;
    for (const auto& builtin : claude_builtins) {
        const auto name = QString::fromLatin1(builtin.name);
        seen.insert(name);
        out.append(
            QVariantMap{{QStringLiteral("name"), name},
                        {QStringLiteral("description"), QString::fromUtf8(builtin.description)},
                        {QStringLiteral("source"), QStringLiteral("claude")}});
    }
    // The project's own commands win over the person's of the same name, as
    // Claude Code resolves them.
    if (!folder.isEmpty())
        add_commands(out, seen, QDir(folder).filePath(QStringLiteral(".claude")),
                     QStringLiteral("project"));
    if (!home.isEmpty())
        add_commands(out, seen, QDir(home).filePath(QStringLiteral(".claude")),
                     QStringLiteral("user"));
    return out;
}

QVariantList codex_slash_commands([[maybe_unused]] const QString& home,
                                  [[maybe_unused]] const QString& folder) {
    QSet<QString> seen;
    return terminal_builtins(codex_builtins, QStringLiteral("codex"), seen);
}

QVariantList agy_slash_commands(const QString& home, const QString& folder) {
    QSet<QString> seen;
    auto out = terminal_builtins(agy_builtins, QStringLiteral("agy"), seen);
    // The project's skills, nearest folder first, win over the person's, as
    // agy loads them; it looks from the folder up to the repository root.
    if (!folder.isEmpty()) {
        QStringList dirs;
        QDir dir(folder);
        bool repository = false;
        for (int depth = 0; depth < 64; ++depth) {
            dirs.append(dir.absolutePath());
            if (dir.exists(QStringLiteral(".git"))) {
                repository = true;
                break;
            }
            if (!dir.cdUp())
                break;
        }
        if (!repository)
            dirs = {QDir(folder).absolutePath()};
        for (const auto& path : dirs)
            for (const char* root : {".agents", ".agent", "_agents", "_agent"})
                add_skills(
                    out, seen,
                    QDir(path).filePath(QString::fromLatin1(root) + QStringLiteral("/skills")),
                    QStringLiteral("project"), true);
    }
    if (!home.isEmpty())
        add_skills(out, seen, QDir(home).filePath(QStringLiteral(".gemini/config/skills")),
                   QStringLiteral("user"), true);
    return out;
}

QVariantList slash_commands(const QString& harness, const QString& home, const QString& folder) {
    if (harness == QLatin1String("claude"))
        return claude_slash_commands(home, folder);
    if (harness == QLatin1String("codex"))
        return codex_slash_commands(home, folder);
    if (harness == QLatin1String("agy"))
        return agy_slash_commands(home, folder);
    return {};
}
} // namespace lapis::desktop
