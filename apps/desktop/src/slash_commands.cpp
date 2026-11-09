#include "slash_commands.hpp"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QSet>
#include <QVariantMap>

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

// A Markdown file's front matter value for `key`, or empty.
QString front_matter(const QString& text, const QString& key) {
    if (!text.startsWith(QLatin1String("---")))
        return {};
    const auto end = text.indexOf(QLatin1String("\n---"), 3);
    const auto head = text.mid(3, end < 0 ? 0 : end - 3);
    for (const auto& line : head.split(QLatin1Char('\n'))) {
        const auto trimmed = line.trimmed();
        if (trimmed.startsWith(key + QLatin1Char(':'))) {
            auto value = trimmed.mid(key.size() + 1).trimmed();
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
    const QDir skills(root + QStringLiteral("/skills"));
    for (const auto& entry : skills.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        const auto path = skills.filePath(entry + QStringLiteral("/SKILL.md"));
        if (!QFile::exists(path))
            continue;
        QFile file(path);
        QString name = entry;
        if (file.open(QIODevice::ReadOnly)) {
            const auto given =
                front_matter(QString::fromUtf8(file.read(4096)), QStringLiteral("name"));
            if (!given.isEmpty())
                name = given;
        }
        if (seen.contains(name))
            continue;
        seen.insert(name);
        out.append(QVariantMap{{QStringLiteral("name"), name},
                               {QStringLiteral("description"), description_of(path)},
                               {QStringLiteral("source"), source}});
    }
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
} // namespace lapis::desktop
