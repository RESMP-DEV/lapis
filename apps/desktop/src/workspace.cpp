#include "workspace.hpp"
#include "agent_checkpoint.hpp"
#include "app_paths.hpp"
#include "live_connection.hpp"
#include "platform/posix/local_endpoint.hpp"
#include "workspace_control.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QPointer>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QThreadPool>
#include <QUuid>
#include <algorithm>
#include <array>
#include <iterator>
#include <stdexcept>
#include <utility>

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>

namespace lapis::desktop {
namespace {
struct Harness {
    const char* id;
    const char* label;
    const char* command;
    // The CLI's own non-interactive update command, if lapis runs it. Codex
    // has none: its observer accepts only qualified binaries, so lapis keeps
    // the qualified build and turns off Codex's update prompt instead.
    const char* update;
    // Offered in the new-agent pickers; a retired CLI is kept only so saved
    // agents of it still restore.
    bool offered;
};
// In the order the pickers offer them.
constexpr std::array harness_catalog{Harness{"claude", "Claude", "claude", "update", true},
                                     Harness{"codex", "Codex", "codex", nullptr, true},
                                     Harness{"opencode", "OpenCode", "opencode", "upgrade", true},
                                     Harness{"grok", "Grok", "grok", "update", true},
                                     Harness{"omp", "OMP", "omp", "update", true},
                                     Harness{"agy", "Antigravity", "agy", "update", true},
                                     Harness{"kimi", "Kimi", "kimi", "upgrade", true},
                                     Harness{"gemini", "Gemini", "gemini", nullptr, false}};
// Arguments lapis always gives a new agent of a CLI, before the user's own.
QStringList defaultArguments(const QString& harness) {
    if (harness == QLatin1String("codex"))
        return {QStringLiteral("-c"), QStringLiteral("check_for_update_on_startup=false")};
    return {};
}
QString resumeOption(const QString& harness);

bool managedResumeMatches(const QStringList& arguments, qsizetype index, const QString& option,
                          const QString& identity) {
    return index >= 0 && index + 1 < arguments.size() && arguments.at(index) == option &&
           arguments.at(index + 1) == identity;
}

QString harness_id(session::AgentMode mode) {
    switch (mode) {
    case session::AgentMode::codex:
        return QStringLiteral("codex");
    case session::AgentMode::claude:
        return QStringLiteral("claude");
    case session::AgentMode::terminal:
        return {};
    }
    return {};
}

const Harness* findHarness(const QString& id) {
    const auto found = std::find_if(harness_catalog.begin(), harness_catalog.end(),
                                    [&](const auto& item) { return id == QLatin1String(item.id); });
    return found == harness_catalog.end() ? nullptr : &*found;
}
QString harnessExecutable(const Harness& harness) {
    auto paths = qEnvironmentVariable("PATH").split(QDir::listSeparator(), Qt::SkipEmptyParts);
    for (const auto* suffix :
         {"/.local/bin", "/.bun/bin", "/.grok/bin", "/.kimi-code/bin", "/.opencode/bin", "/bin"})
        paths.append(QDir::homePath() + QLatin1String(suffix));
    paths << QStringLiteral("/opt/homebrew/bin") << QStringLiteral("/usr/local/bin");
    return QStandardPaths::findExecutable(QLatin1String(harness.command), paths);
}
// Three approval modes, and the flags each CLI takes for them (checked
// against each CLI's --help, September 24). A CLI is offered only those it
// has: OMP, OpenCode and Antigravity have no Auto, and Kimi and OpenCode no
// Accept edits.
struct ModeFlags {
    const char* harness;
    const char* mode;
    std::array<const char*, 4> flags;
};
constexpr std::array mode_flags{
    // Codex applies edits in the workspace without asking in on-request.
    ModeFlags{"codex", "edits", {"-a", "on-request", "-s", "workspace-write"}},
    ModeFlags{"codex", "auto", {"-a", "never", "-s", "workspace-write"}},
    ModeFlags{"codex", "full", {"--dangerously-bypass-approvals-and-sandbox"}},
    ModeFlags{"claude", "edits", {"--permission-mode", "acceptEdits"}},
    ModeFlags{"claude", "auto", {"--permission-mode", "auto"}},
    ModeFlags{"claude", "full", {"--permission-mode", "bypassPermissions"}},
    ModeFlags{"grok", "edits", {"--permission-mode", "acceptEdits"}},
    ModeFlags{"grok", "auto", {"--permission-mode", "auto"}},
    ModeFlags{"grok", "full", {"--permission-mode", "bypassPermissions"}},
    ModeFlags{"kimi", "auto", {"--yolo"}},
    ModeFlags{"kimi", "full", {"--auto"}},
    ModeFlags{"omp", "edits", {"--approval-mode=write"}},
    ModeFlags{"omp", "full", {"--approval-mode=yolo"}},
    ModeFlags{"opencode", "full", {"--auto"}},
    ModeFlags{"agy", "edits", {"--mode", "accept-edits"}},
    ModeFlags{"agy", "full", {"--dangerously-skip-permissions"}},
};
constexpr std::array<std::pair<const char*, const char*>, 3> mode_names{
    {{"edits", "Accept edits"}, {"auto", "Auto"}, {"full", "Full access"}}};
QStringList modeArguments(const QString& harness, const QString& mode) {
    for (const auto& entry : mode_flags)
        if (harness == QLatin1String(entry.harness) && mode == QLatin1String(entry.mode)) {
            QStringList flags;
            for (const auto* flag : entry.flags)
                if (flag)
                    flags << QString::fromLatin1(flag);
            return flags;
        }
    return {};
}
QVariantList harnessModes(const QString& harness) {
    QVariantList modes;
    for (const auto& [mode, name] : mode_names)
        if (!modeArguments(harness, QString::fromLatin1(mode)).isEmpty())
            modes.append(QVariantMap{{QStringLiteral("id"), QString::fromLatin1(mode)},
                                     {QStringLiteral("name"), QString::fromLatin1(name)}});
    return modes;
}
// The CLI's model flag with a model name; empty when it has none here. The
// CLI id and the model are both strings; call sites name each.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
QStringList modelArguments(const QString& harness, const QString& model) {
    if (model.isEmpty())
        return {};
    if (harness == QLatin1String("omp"))
        return {QStringLiteral("--model=") + model};
    if (harness == QLatin1String("claude") || harness == QLatin1String("agy"))
        return {QStringLiteral("--model"), model};
    if (harness == QLatin1String("codex") || harness == QLatin1String("grok") ||
        harness == QLatin1String("kimi") || harness == QLatin1String("opencode"))
        return {QStringLiteral("-m"), model};
    return {};
}
bool validModel(const QString& model) {
    static const QRegularExpression name(
        QStringLiteral(R"(^[A-Za-z0-9][A-Za-z0-9._:/\[\]@+-]{0,127}$)"));
    return model.isEmpty() || name.match(model).hasMatch();
}

// One POSIX shell word, whatever it holds.
QString shellWord(const QString& text) {
    static const QRegularExpression plain(QStringLiteral(R"(^[A-Za-z0-9_./=:@%+,-]+$)"));
    if (plain.match(text).hasMatch())
        return text;
    auto quoted = text;
    quoted.replace(QLatin1Char('\''), QStringLiteral(R"('\'')"));
    return QLatin1Char('\'') + quoted + QLatin1Char('\'');
}
// A folder on another machine; ~ stays that machine's home.
QString remoteFolder(const QString& directory) {
    if (directory == QStringLiteral("~"))
        return directory;
    if (directory.startsWith(QStringLiteral("~/")))
        return QStringLiteral("~/") + shellWord(directory.mid(2));
    return shellWord(directory);
}
// An ssh host name as ssh config and the phone name it; never an option.
bool validMachine(const QString& machine) {
    static const QRegularExpression name(QStringLiteral(R"(^[A-Za-z0-9][A-Za-z0-9._:-]{0,127}$)"));
    return name.match(machine).hasMatch();
}
constexpr std::string_view kPreviewPalette =
    "\x1b]10;rgb:d9/de/e8\x1b\\\x1b]11;rgb:0d/13/1d\x1b\\"
    "\x1b]4;2;rgb:87/cb/ac\x1b\\\x1b]4;4;rgb:9c/b4/ee\x1b\\"
    "\x1b]4;3;rgb:df/bb/7b\x1b\\\x1b]4;5;rgb:ba/a4/e8\x1b\\"
    "\x1b]4;8;rgb:75/83/98\x1b\\";

// An agent on another machine: ssh runs the CLI in an interactive login shell
// there, so its PATH matches that machine's terminal. Returns why not, or
// empty with `launch` set.
QString remoteLaunch(const AgentRequest& request, const QString& command,
                     const QStringList& arguments, std::optional<session::LaunchSpec>& launch) {
    const auto ssh = QStandardPaths::findExecutable(QStringLiteral("ssh"));
    if (ssh.isEmpty())
        return QStringLiteral("ssh is not available on this Mac.");
    if (!validMachine(request.machine))
        return QStringLiteral("Unknown machine name.");
    if (request.program.contains(QChar::Null) || request.program.contains(QLatin1Char('\n')))
        return QStringLiteral("Invalid program path.");
    QStringList words{shellWord(request.program.isEmpty() ? command : request.program)};
    for (const auto& argument : arguments)
        words << shellWord(argument);
    const auto line = QStringLiteral(R"(cd %1 && exec "${SHELL:-/bin/sh}" -lic %2)")
                          .arg(remoteFolder(request.directory), shellWord(words.join(' ')));
    launch = session::validate_launch({ssh,
                                       {QStringLiteral("-t"), request.machine, line},
                                       QDir::homePath(),
                                       {100, 30},
                                       session::AgentMode::terminal});
    return {};
}
constexpr qsizetype max_saved_arguments = 64;
} // namespace

QString harness_program(const QString& id) {
    const auto* harness = findHarness(id);
    return harness ? harnessExecutable(*harness) : QString();
}

SessionPreview::SessionPreview(QString title, QString directory, QString activity, QColor accent,
                               std::string_view content)
    : title_(std::move(title)), directory_(std::move(directory)), activity_(std::move(activity)),
      accent_(accent) {
    connect(this, &SessionPreview::connectionChanged, this, &SessionPreview::statusChanged);
    connect(this, &SessionPreview::attentionChanged, this, &SessionPreview::statusChanged);
    quiet_timer_.setSingleShot(true);
    connect(&quiet_timer_, &QTimer::timeout, this, [this] {
        if (!output_active_)
            return;
        output_active_ = false;
        output_quiet_ = true;
        emit statusChanged();
    });
    session::Terminal terminal({100, 30});
    // Only fixtures get the sample palette. A live agent's placeholder keeps
    // the engine defaults its first real frame will have, so nothing flashes
    // navy before it connects or while it is unreachable.
    if (!content.empty())
        terminal.feed(kPreviewPalette);
    terminal.feed(content);
    snapshot_ = terminal.snapshot();
}

QString Workspace::rootDirectory() { return data_directory(); }

QString Workspace::defaultEndpoint() {
    return QDir{rootDirectory()}.filePath(QStringLiteral("runtime/desktop-v6.sock"));
}

Workspace::~Workspace() {
    if (headless_ && registry_lock_ && registry_lock_->isLocked())
        QFile::remove(storage_path_ + QStringLiteral(".restoring"));
}

Workspace::Workspace(WorkspaceMode mode, WorkspaceOptions options)
    : restore_agents_(options.restoreAgents), update_harnesses_(options.updateHarnesses),
      headless_(options.headless), preview_mode_(mode == WorkspaceMode::preview) {
    // Selecting an agent is looking at it.
    connect(this, &Workspace::focusChanged, this, [this] {
        if (auto* focused = focusedSession())
            focused->setUnseen(false);
    });
    const auto add = [this](const char* title, const char* directory, const char* activity,
                            const char* accent, std::string_view content) {
        sessions_.push_back(std::make_unique<SessionPreview>(
            QString::fromUtf8(title), QString::fromUtf8(directory), QString::fromUtf8(activity),
            QColor(QString::fromLatin1(accent)), content));
    };
    if (preview_mode_ && (options.launch || !options.endpoint.isEmpty()))
        throw std::invalid_argument("UI preview cannot launch or attach to a process");
    categories_.push_back({QStringLiteral("general"), QStringLiteral("General"), {}, {}});
    if (!preview_mode_) {
        if (options.launch || !options.endpoint.isEmpty()) {
            const auto launch = options.launch ? session::validate_launch(*options.launch)
                                               : session::shell_launch(rootDirectory());
            add("Agent", launch.directory.toUtf8().constData(), "Connecting", "#87cbac", "");
            sessions_.front()->setSessionId(QStringLiteral("shell"));
            const auto harness = harness_id(launch.agent);
            const Agent agent{active_category_, options.endpoint, launch, harness};
            sessions_.front()->setHarnessId(harness);
            sessions_.front()->setStatusSource(statusSource(agent));
            agents_.insert(QStringLiteral("shell"), agent);
            sessions_.front()->startLive(session::posix::prepare_endpoint(options.endpoint.isEmpty()
                                                                              ? defaultEndpoint()
                                                                              : options.endpoint),
                                         launch, options.mode);
            watch(sessions_.front().get());
            restoreSelection();
            return;
        }
        storage_path_ =
            options.storagePath.isEmpty()
                ? QDir(rootDirectory()).filePath(QStringLiteral("runtime/workspace.json"))
                : QFileInfo(options.storagePath).absoluteFilePath();
        restore();
        restoreSelection();
        return;
    }
    add("Codex", "lapis", "UI preview", "#87cbac", "~/lapis\r\n\r\n> Ready for the next step.\r\n");
    add("Renderer", "lapis/apps/desktop", "In progress", "#9cb4ee",
        "\x1b[35mlapis\x1b[0m  \x1b[90mapps/desktop\x1b[0m\r\n\r\n"
        "\x1b[1mTerminal surface\x1b[0m\r\n\r\n"
        "  \x1b[34m01\x1b[0m  Keep text crisp at native display scale\r\n"
        "  \x1b[34m02\x1b[0m  Draw from retained terminal snapshots\r\n"
        "  \x1b[34m03\x1b[0m  Let unchanged scenes sleep\r\n\r\n"
        "\x1b[90m// Preview content, not an active agent transcript.\x1b[0m\r\n\r\n"
        "\x1b[35mvoid\x1b[0m TerminalSurface::update() {\r\n"
        "    \x1b[90m// Present the selected session.\x1b[0m\r\n"
        "    draw(snapshot);\r\n"
        "}\r\n\r\n\x1b[34m>\x1b[0m ");
    add("Agent bridge", "lapis/adapters", "Needs input", "#dfbb7b",
        "\x1b[35mlapis\x1b[0m  \x1b[90madapters/codex\x1b[0m\r\n\r\n"
        "\x1b[1mAttention without interruption\x1b[0m\r\n\r\n"
        "  A session can request attention.\r\n"
        "  The workspace keeps keyboard ownership explicit.\r\n\r\n"
        "\x1b[33m  SAMPLE REQUEST\x1b[0m\r\n"
        "  Review the adapter contract before continuing.\r\n\r\n"
        "\x1b[90m  Focusing a session does not approve its request.\x1b[0m\r\n"
        "\x1b[90m  This card illustrates the attention treatment.\x1b[0m\r\n");
    add("Session service", "lapis/services", "Queued", "#92a0b5",
        "\x1b[35mlapis\x1b[0m  \x1b[90mservices/session\x1b[0m\r\n\r\n"
        "\x1b[1mNext implementation slice\x1b[0m\r\n\r\n"
        "  [ ] Launch a real child under a POSIX PTY\r\n"
        "  [ ] Route input, output and terminal resize\r\n"
        "  [ ] Handle exit and descriptor cleanup\r\n"
        "  [ ] Preserve sessions across GUI attachment\r\n\r\n"
        "\x1b[90m  Preview content, not a running agent.\x1b[0m\r\n");
    add("Checks", "lapis", "Passed", "#87cbac",
        "\x1b[35mlapis\x1b[0m  \x1b[90mterminal adapter\x1b[0m\r\n\r\n"
        "\x1b[34m>\x1b[0m just asan\r\n\r\n"
        "  \x1b[32mPASS\x1b[0m  snapshot lifetime\r\n"
        "  \x1b[32mPASS\x1b[0m  bounded reply overflow\r\n"
        "  \x1b[32mPASS\x1b[0m  history eviction and clearing\r\n"
        "  \x1b[32mPASS\x1b[0m  input rejection and recovery\r\n\r\n"
        "\x1b[90m  Saved adapter results; not a live test runner.\x1b[0m\r\n");
    add("Workspace notes", "lapis", "Idle", "#92a0b5",
        "\x1b[35mlapis\x1b[0m  \x1b[90mworkspace notes\x1b[0m\r\n\r\n"
        "\x1b[1mResponsiveness. Ergonomics. Visuals.\x1b[0m\r\n\r\n"
        "  Keep the focused session easy to read.\r\n"
        "  Keep neighboring sessions recognizable.\r\n"
        "  Use color for attention, not decoration.\r\n"
        "  Leave enough quiet space to think.\r\n\r\n"
        "\x1b[90m  Preview cards show the planned layout.\x1b[0m\r\n");
    const std::array ids{"shell", "renderer", "agent", "service", "checks", "notes"};
    if (sessions_.size() != ids.size())
        throw std::logic_error("session card count does not match the id table");
    for (std::size_t i = 0; i < sessions_.size(); ++i)
        sessions_[i]->setSessionId(QString::fromLatin1(ids[i]));
    for (const auto& item : sessions_) {
        agents_.insert(item->sessionId(), {active_category_, {}, {}});
        watch(item.get());
    }
    restoreSelection();
}

QVariantList Workspace::sessions() const {
    QVariantList result;
    result.reserve(static_cast<qsizetype>(sessions_.size()));
    for (const auto& session : sessions_)
        result.push_back(QVariant::fromValue(session.get()));
    return result;
}

SessionPreview* Workspace::focusedSession() const {
    return focused_index_ < 0 ? nullptr
                              : sessions_.at(static_cast<std::size_t>(focused_index_)).get();
}

void Workspace::nextSession(int delta) {
    const auto list = categorySessions();
    if (list.isEmpty())
        return;
    int current = 0;
    for (int i = 0; i < list.size(); ++i)
        if (list[i].value<SessionPreview*>() == focusedSession())
            current = i;
    const int count = static_cast<int>(list.size());
    const int next = ((current + delta % count) % count + count) % count;
    selectSession(list[next].value<SessionPreview*>()->sessionId());
}

bool Workspace::nextAttention() {
    // Category order, then strip order, starting after the selected agent.
    std::vector<SessionPreview*> order;
    for (const auto& category : categories_)
        for (const auto& item : sessions_)
            if (agents_.value(item->sessionId()).category == category.id)
                order.push_back(item.get());
    auto* const focused = focusedSession();
    const auto current = std::find(order.begin(), order.end(), focused);
    const std::size_t start =
        current == order.end() ? order.size() : static_cast<std::size_t>(current - order.begin());
    for (const bool requests : {true, false})
        for (std::size_t step = 1; step <= order.size(); ++step) {
            auto* candidate = order[(start + step) % order.size()];
            if (candidate != focused &&
                (requests ? candidate->attentionCount() > 0 : candidate->unseen()))
                return selectSession(candidate->sessionId());
        }
    return false;
}
bool SessionPreview::addPreviewRequest(const QString& id, const QString& reason) {
    if (id.isEmpty() || id.size() > 64 || reason.size() > 256 || requests_.contains(id) ||
        requests_.size() >= 8)
        return false;
    requests_.insert(id, reason);
    ++attention_serial_;
    emit attentionChanged();
    emit attentionArrived();
    return true;
}
bool SessionPreview::resolvePreviewRequest(const QString& id) {
    if (requests_.remove(id) == 0)
        return false;
    emit attentionChanged();
    return true;
}
void SessionPreview::clearPreviewRequests() {
    if (requests_.isEmpty())
        return;
    requests_.clear();
    emit attentionChanged();
}
SessionPreview* Workspace::session(const QString& id) const {
    for (const auto& item : sessions_)
        if (item->sessionId() == id)
            return item.get();
    return nullptr;
}
bool Workspace::requestAttention(const PreviewRequest& request) {
    auto* target = session(request.session_id);
    return preview_mode_ && target && target->addPreviewRequest(request.request_id, request.reason);
}
bool Workspace::resolveAttention(const PreviewRequest& request) {
    auto* target = session(request.session_id);
    return preview_mode_ && target && target->resolvePreviewRequest(request.request_id);
}
bool Workspace::replayAttention(const QString& scenario) {
    if (!preview_mode_)
        return false;
    if (scenario == QStringLiteral("arrival") || scenario == QStringLiteral("duplicate"))
        return requestAttention({QStringLiteral("agent"), QStringLiteral("request-1"),
                                 QStringLiteral("Review the next step")});
    if (scenario == QStringLiteral("two")) {
        static_cast<void>(requestAttention({QStringLiteral("agent"), QStringLiteral("request-1"),
                                            QStringLiteral("Review the next step")}));
        return requestAttention({QStringLiteral("renderer"), QStringLiteral("request-2"),
                                 QStringLiteral("Choose the rendering option")});
    }
    if (scenario == QStringLiteral("resolve"))
        return resolveAttention({QStringLiteral("agent"), QStringLiteral("request-1"), {}});
    if (scenario == QStringLiteral("reset")) {
        for (const auto& item : sessions_)
            item->clearPreviewRequests();
        return true;
    }
    return false;
}

namespace {
bool validName(const QString& name) {
    return !name.trimmed().isEmpty() && name.size() <= 80 &&
           std::none_of(name.begin(), name.end(),
                        [](QChar ch) { return ch.isNull() || !ch.isPrint(); });
}
QString newId() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
} // namespace
bool Workspace::fail(const QString& message) {
    error_ = message;
    emit errorChanged();
    return false;
}
void Workspace::clearError() {
    error_.clear();
    emit errorChanged();
}
void Workspace::watch(SessionPreview* item) {
    connect(item, &SessionPreview::connectionChanged, this, [this, item] { finishClosing(item); });
    connect(item, &SessionPreview::attentionArrived, this, &Workspace::requestArrived);
    connect(item, &SessionPreview::attentionArrived, this,
            [this, item] { emit agentNeedsYou(item); });
    last_kind_.insert(item, item->statusKind());
    connect(item, &SessionPreview::statusChanged, this, [this, item] { noteStatus(item); });
    connect(item, &SessionPreview::unseenChanged, this, [this] {
        if (!batching_categories_)
            emit categoriesChanged();
    });
    // A new request marks an unselected agent; the category's request count
    // and its unseen count change together, so publish them once.
    connect(item, &SessionPreview::attentionChanged, this,
            [this, item, count = item->attentionCount()]() mutable {
                const int updated = item->attentionCount();
                if (updated == count)
                    return;
                batching_categories_ = true;
                if (updated > count && item != focusedSession())
                    item->setUnseen(true);
                batching_categories_ = false;
                count = updated;
                emit categoriesChanged();
            });
}
int Workspace::attentionAgents() const {
    return static_cast<int>(std::count_if(sessions_.begin(), sessions_.end(), [](const auto& item) {
        return item->unseen() || item->attentionCount() > 0;
    }));
}
QVariantList Workspace::categories() const {
    QVariantList result;
    for (const auto& category : categories_) {
        int count = 0;
        int unseen = 0;
        for (const auto& item : sessions_)
            if (agents_.value(item->sessionId()).category == category.id) {
                count += item->attentionCount();
                unseen += item->unseen() ? 1 : 0;
            }
        result.append(QVariantMap{{"id", category.id},
                                  {"name", category.name},
                                  {"attentionCount", count},
                                  {"unseenCount", unseen}});
    }
    return result;
}
QVariantList Workspace::categorySessions() const {
    QVariantList result;
    for (const auto& item : sessions_)
        if (agents_.value(item->sessionId()).category == active_category_)
            result.append(QVariant::fromValue(item.get()));
    return result;
}
void Workspace::restoreSelection() {
    focused_index_ = -1;
    auto category = std::find_if(categories_.begin(), categories_.end(),
                                 [&](const auto& value) { return value.id == active_category_; });
    if (category == categories_.end())
        return;
    // With tiles on the stage, the selected agent is one of them.
    if (!category->tiles.empty() && !category->tiles.contains(category->selected))
        category->selected = category->tiles.sessions().front();
    for (std::size_t i = 0; i < sessions_.size(); ++i) {
        const auto& id = sessions_[i]->sessionId();
        if (agents_.value(id).category != active_category_)
            continue;
        if (focused_index_ < 0 || id == category->selected)
            focused_index_ = static_cast<int>(i);
        if (id == category->selected)
            break;
    }
    category->selected = focusedSession() ? focusedSession()->sessionId() : QString{};
}
void Workspace::changed() {
    restoreSelection();
    emit categoriesChanged();
    emit categoryChanged();
    emit sessionsChanged();
    emit tilesChanged();
    emit focusChanged();
}
Workspace::RegistryState Workspace::checkpoint() const {
    return {categories_, agents_, active_category_, focused_index_};
}
void Workspace::rollback(const RegistryState& previous) {
    categories_ = previous.categories;
    agents_ = previous.agents;
    active_category_ = previous.active;
    focused_index_ = previous.focused;
}
bool Workspace::mutableRegistry() {
    return !storage_failed_ ||
           fail(QStringLiteral("Repair the workspace registry before changing it."));
}
bool Workspace::commit(const RegistryState& previous) {
    restoreSelection();
    if (save())
        return true;
    rollback(previous);
    emit errorChanged();
    return false;
}
bool Workspace::addCategory(const QString& name) { return !insertCategory(name, true).isEmpty(); }

// From the phone: the category is added without moving the window to it.
QString Workspace::createCategory(const QString& name) { return insertCategory(name, false); }

QString Workspace::insertCategory(const QString& name, bool select) {
    if (!mutableRegistry())
        return {};
    if (!validName(name) || categories_.size() >= 32)
        return failed(QStringLiteral(
            "Use a category name of 1–80 characters; at most 32 categories are supported."));
    const auto previous = checkpoint();
    categories_.push_back({newId(), name.trimmed(), {}, {}});
    auto id = categories_.back().id;
    if (select)
        active_category_ = id;
    if (!commit(previous))
        return {};
    changed();
    return id;
}
// QML positional API v1 requires QString arguments; role names and boundary validation are
// explicit. NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
bool Workspace::renameCategory(const QString& id, const QString& name) {
    if (!mutableRegistry())
        return false;
    if (!validName(name))
        return fail(QStringLiteral("Use a category name of 1–80 characters."));
    for (auto& category : categories_)
        if (category.id == id) {
            const auto previous = checkpoint();
            category.name = name.trimmed();
            if (!commit(previous))
                return false;
            changed();
            return true;
        }
    return fail(QStringLiteral("Category no longer exists."));
}
bool Workspace::removeCategory(const QString& id) {
    if (!mutableRegistry())
        return false;
    if (categories_.size() == 1)
        return fail(QStringLiteral("Keep at least one category."));
    for (const auto& agent : agents_)
        if (agent.category == id)
            return fail(QStringLiteral("Move the agents out before removing this category."));
    const auto previous = checkpoint();
    const auto removed =
        std::erase_if(categories_, [&](const auto& category) { return category.id == id; });
    if (removed == 0)
        return fail(QStringLiteral("Category no longer exists."));
    if (active_category_ == id)
        active_category_ = categories_.front().id;
    if (!commit(previous))
        return false;
    changed();
    return true;
}
bool Workspace::selectCategory(const QString& id) {
    if (!mutableRegistry())
        return false;
    if (std::none_of(categories_.begin(), categories_.end(),
                     [&](const auto& category) { return category.id == id; }))
        return false;
    if (active_category_ == id)
        return true;
    const auto previous = checkpoint();
    active_category_ = id;
    if (!commit(previous))
        return false;
    emit categoryChanged();
    emit sessionsChanged();
    emit tilesChanged();
    emit focusChanged();
    return true;
}
void Workspace::nextCategory(int delta) {
    const int count = static_cast<int>(categories_.size());
    for (int i = 0; i < count; ++i)
        if (categories_[static_cast<std::size_t>(i)].id == active_category_) {
            selectCategory(
                categories_[static_cast<std::size_t>(((i + delta % count) % count + count) % count)]
                    .id);
            return;
        }
}
bool Workspace::selectSession(const QString& id) {
    if (!mutableRegistry())
        return false;
    if (!session(id))
        return false;
    if (focusedSession() && focusedSession()->sessionId() == id)
        return true;
    const auto previous = checkpoint();
    const bool category_changed = active_category_ != agents_.value(id).category;
    active_category_ = agents_.value(id).category;
    bool retiled = false;
    for (auto& category : categories_)
        if (category.id == active_category_) {
            // The selected tile shows the agent picked from the strip.
            if (!category.tiles.empty() && !category.tiles.contains(id))
                retiled = category.tiles.replace(category.selected, id);
            category.selected = id;
        }
    if (!commit(previous))
        return false;
    if (category_changed) {
        emit categoryChanged();
        emit sessionsChanged();
    }
    if (category_changed || retiled)
        emit tilesChanged();
    emit focusChanged();
    return true;
}
// QML positional API v1 requires QString arguments; role names and boundary validation are
// explicit. NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
bool Workspace::renameSession(const QString& id, const QString& title) {
    if (!mutableRegistry())
        return false;
    auto* item = session(id);
    if (!item || !validName(title))
        return fail(QStringLiteral("Use an agent name of 1–80 characters."));
    if (!save(id, title.trimmed())) {
        emit errorChanged();
        return false;
    }
    item->rename(title.trimmed());
    return true;
}
bool Workspace::moveSession(const QString& id, const QString& categoryId) {
    if (!mutableRegistry())
        return false;
    if (!session(id) ||
        std::none_of(categories_.begin(), categories_.end(),
                     [&](const auto& category) { return category.id == categoryId; }))
        return fail(QStringLiteral("Agent or category no longer exists."));
    const auto previous = checkpoint();
    const auto entry = agents_.find(id);
    if (entry == agents_.end())
        return fail(QStringLiteral("Missing agent metadata."));
    for (auto& category : categories_) {
        if (category.id == entry->category)
            untile(category, id);
        if (category.selected == id)
            category.selected.clear();
    }
    entry->category = categoryId;
    if (!commit(previous))
        return false;
    changed();
    return true;
}
bool Workspace::moveSessionBy(const QString& id, int delta) {
    if (!mutableRegistry())
        return false;
    if (!session(id) || delta == 0)
        return false;
    std::vector<std::size_t> indices;
    for (std::size_t i = 0; i < sessions_.size(); ++i)
        if (agents_.value(sessions_[i]->sessionId()).category == agents_.value(id).category)
            indices.push_back(i);
    for (std::size_t i = 0; i < indices.size(); ++i)
        if (sessions_[indices[i]]->sessionId() == id) {
            const auto previous = checkpoint();
            const auto target = std::clamp(static_cast<qint64>(i) + delta, qint64{0},
                                           static_cast<qint64>(indices.size()) - 1);
            const auto step = target > static_cast<qint64>(i) ? 1 : -1;
            const auto swapPositions = [&](qint64 from, qint64 to) {
                std::swap(sessions_[indices[static_cast<std::size_t>(from)]],
                          sessions_[indices[static_cast<std::size_t>(to)]]);
            };
            for (qint64 position = static_cast<qint64>(i); position != target; position += step)
                swapPositions(position, position + step);
            restoreSelection();
            if (!save()) {
                for (qint64 position = target; position != static_cast<qint64>(i); position -= step)
                    swapPositions(position, position - step);
                rollback(previous);
                emit errorChanged();
                return false;
            }
            changed();
            return true;
        }
    return false;
}
bool Workspace::placeSessions(const QStringList& ids, const QString& categoryId, int index) {
    if (!mutableRegistry())
        return false;
    auto* destination = category(categoryId);
    const QSet<QString> moving(ids.begin(), ids.end());
    if (destination == nullptr || moving.isEmpty() || moving.size() != ids.size() ||
        std::any_of(ids.begin(), ids.end(), [&](const auto& id) { return !session(id); }))
        return fail(QStringLiteral("Agent or category no longer exists."));
    const auto previous = checkpoint();
    std::vector<SessionPreview*> old_order;
    old_order.reserve(sessions_.size());
    for (const auto& item : sessions_)
        old_order.push_back(item.get());
    std::vector<std::unique_ptr<SessionPreview>> moved;
    std::vector<std::unique_ptr<SessionPreview>> rest;
    for (auto& item : sessions_)
        (moving.contains(item->sessionId()) ? moved : rest).push_back(std::move(item));
    const auto position = insertionPoint(rest, categoryId, index);
    rest.insert(rest.begin() + static_cast<std::ptrdiff_t>(position),
                std::make_move_iterator(moved.begin()), std::make_move_iterator(moved.end()));
    sessions_ = std::move(rest);
    recategorize(ids, categoryId);
    restoreSelection();
    if (!save()) {
        std::vector<std::unique_ptr<SessionPreview>> restored;
        for (auto* item : old_order) {
            const auto found = std::find_if(sessions_.begin(), sessions_.end(),
                                            [&](const auto& value) { return value.get() == item; });
            restored.push_back(std::move(*found));
        }
        sessions_ = std::move(restored);
        rollback(previous);
        emit errorChanged();
        return false;
    }
    changed();
    return true;
}
std::size_t Workspace::insertionPoint(const std::vector<std::unique_ptr<SessionPreview>>& staying,
                                      const QString& categoryId, int index) const {
    std::optional<std::size_t> last;
    int seen = 0;
    for (std::size_t i = 0; i < staying.size(); ++i) {
        if (agents_.value(staying[i]->sessionId()).category != categoryId)
            continue;
        if (seen++ == index)
            return i;
        last = i;
    }
    return last ? *last + 1 : staying.size();
}
void Workspace::recategorize(const QStringList& ids, const QString& categoryId) {
    for (const auto& id : ids) {
        auto& agent = agents_[id];
        if (agent.category == categoryId)
            continue;
        for (auto& place : categories_) {
            if (place.id == agent.category)
                untile(place, id);
            if (place.selected == id)
                place.selected.clear();
        }
        agent.category = categoryId;
    }
}
bool Workspace::placeCategory(const QString& id, int index) {
    if (!mutableRegistry())
        return false;
    const auto found = std::find_if(categories_.begin(), categories_.end(),
                                    [&](const auto& value) { return value.id == id; });
    if (found == categories_.end())
        return fail(QStringLiteral("Category no longer exists."));
    const auto previous = checkpoint();
    auto moved = std::move(*found);
    categories_.erase(found);
    const auto target = std::clamp<qsizetype>(index, 0, static_cast<qsizetype>(categories_.size()));
    categories_.insert(categories_.begin() + target, std::move(moved));
    if (!commit(previous))
        return false;
    changed();
    return true;
}
bool Workspace::removeSession(const QString& id) {
    if (!mutableRegistry())
        return false;
    const auto* item = session(id);
    if (!item)
        return false;
    if (item->live() && item->connectionState() != QStringLiteral("ended"))
        return fail(QStringLiteral(
            "This agent may still be running. End it in the agent before removing its tab."));
    return discardSession(id);
}
bool Workspace::discardSession(const QString& id) {
    const auto* item = session(id);
    if (!item)
        return false;
    const auto item_category = agents_.value(id).category;
    const auto closed_agent = agents_.value(id);
    const auto closed_title = item->title();
    const auto previous = checkpoint();
    // A closed agent hands focus to its right neighbor in its own category, or
    // to its left one at that category's end, even while another strip is shown.
    QVariantList list;
    for (const auto& candidate : sessions_)
        if (agents_.value(candidate->sessionId()).category == item_category)
            list.append(QVariant::fromValue(candidate.get()));
    for (qsizetype i = 0; i < list.size(); ++i) {
        if (list[i].value<SessionPreview*>() != item)
            continue;
        const auto next =
            list.size() < 2
                ? QString()
                : list[i + 1 < list.size() ? i + 1 : i - 1].value<SessionPreview*>()->sessionId();
        for (auto& category : categories_) {
            if (category.id == item_category)
                untile(category, id);
            if (category.selected == id)
                category.selected = next;
        }
    }
    const auto position = std::find_if(sessions_.begin(), sessions_.end(),
                                       [&](const auto& value) { return value.get() == item; });
    const auto index = std::distance(sessions_.begin(), position);
    auto retained = std::move(*position);
    sessions_.erase(position);
    agents_.remove(id);
    restoreSelection();
    if (!save()) {
        sessions_.insert(sessions_.begin() + index, std::move(retained));
        rollback(previous);
        emit errorChanged();
        return false;
    }
    last_kind_.remove(item);
    rememberClosed(closed_agent, closed_title);
    changed();
    // QML delegates can still hold the removed object during this call stack.
    retained.release()->deleteLater();
    return true;
}
QString Workspace::homeDirectory() const { return QDir::homePath(); }

QString SessionPreview::agentName() const {
    const auto* harness = findHarness(harness_id_);
    return harness ? QString::fromLatin1(harness->label) : QStringLiteral("Agent");
}
void SessionPreview::setHarnessId(const QString& id) {
    if (harness_id_ == id)
        return;
    harness_id_ = id;
    emit identityChanged();
    emit statusChanged();
}
// The models a CLI offers the person; a list in the config replaces the
// rest, with the CLI's default kept first.
std::vector<ModelChoice> Workspace::modelChoices(const QString& harness) const {
    auto found = harness_models_ ? harness_models_->models(harness) : std::vector<ModelChoice>{};
    const auto configured = agent_defaults_.models.value(harness);
    if (configured.isEmpty())
        return found;
    std::vector<ModelChoice> picked;
    const auto fallback = std::find_if(found.begin(), found.end(),
                                       [](const ModelChoice& model) { return model.isDefault; });
    if (fallback != found.end())
        picked.push_back(*fallback);
    for (const auto& name : configured) {
        const auto known = std::find_if(found.begin(), found.end(),
                                        [&](const ModelChoice& model) { return model.id == name; });
        if (fallback == found.end() || fallback->id != name)
            picked.push_back({.id = name,
                              .name = known == found.end() ? name : known->name,
                              .isDefault = false});
    }
    return picked;
}

QVariantList Workspace::availableHarnesses() const {
    QVariantList result;
    for (const auto& harness : harness_catalog) {
        if (!harness.offered)
            continue;
        const auto program = harnessExecutable(harness);
        const auto id = QString::fromLatin1(harness.id);
        QVariantList models;
        if (!modelArguments(id, QStringLiteral("x")).isEmpty())
            for (const auto& model : modelChoices(id))
                models.append(QVariantMap{{QStringLiteral("id"), model.id},
                                          {QStringLiteral("name"), model.name},
                                          {QStringLiteral("default"), model.isDefault}});
        result.append(QVariantMap{{QStringLiteral("id"), id},
                                  {QStringLiteral("name"), QString::fromLatin1(harness.label)},
                                  {QStringLiteral("installed"), !program.isEmpty()},
                                  {QStringLiteral("models"), models},
                                  {QStringLiteral("modes"), harnessModes(id)}});
    }
    return result;
}

QString Workspace::displayPath(const QString& directory) const {
    if (directory.isEmpty())
        return {};
    auto path = QDir::cleanPath(directory);
    const auto home = QDir::cleanPath(QDir::homePath());
    if (path == home)
        return QStringLiteral("~");
    if (path.startsWith(home + QLatin1Char('/')))
        return QStringLiteral("~") + path.mid(home.size());
    return path;
}

// QML positional API v1 requires QString arguments; role names and boundary validation are
// explicit. NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
bool Workspace::createAgent(const QString& directory, const QString& title, const QString& harness,
                            const QString& model, const QString& mode) {
    return !startAgent({.category = active_category_,
                        .directory = directory,
                        .title = title,
                        .harness = harness,
                        .machine = {},
                        .program = {},
                        .model = model,
                        .mode = mode,
                        .select = true})
                .isEmpty();
}
QVariantMap Workspace::agentPlace(const QString& id) const {
    const auto entry = agents_.constFind(id);
    if (entry == agents_.cend())
        return {};
    QString category;
    for (const auto& item : categories_)
        if (item.id == entry->category)
            category = item.name;
    const auto& launch = entry->launch;
    QString machine;
    QString place = displayPath(launch.directory);
    // Remote agents are `ssh -t <host> 'cd <folder> && exec ...'`.
    if (QFileInfo(launch.program).fileName() == QStringLiteral("ssh") &&
        launch.arguments.size() >= 3 && launch.arguments[0] == QStringLiteral("-t")) {
        machine = launch.arguments[1];
        const auto& command = launch.arguments[2];
        const auto end = command.indexOf(QStringLiteral(" && "));
        place = machine + QLatin1Char(':') +
                (command.startsWith(QStringLiteral("cd ")) && end > 3 ? command.mid(3, end - 3)
                                                                      : QString());
    }
    return {{QStringLiteral("category"), category},
            {QStringLiteral("machine"), machine},
            {QStringLiteral("place"), place}};
}
QVariantMap Workspace::agentDefaults() const {
    QVariantMap machines;
    for (auto it = agent_defaults_.machineFolders.begin();
         it != agent_defaults_.machineFolders.end(); ++it)
        machines.insert(it.key(), it.value());
    return {{QStringLiteral("harness"), agent_defaults_.harness},
            {QStringLiteral("folder"), agent_defaults_.folder},
            {QStringLiteral("mode"), agent_defaults_.mode},
            {QStringLiteral("machines"), machines}};
}
std::optional<session::LaunchSpec> Workspace::agentLaunch(const AgentRequest& request) {
    const auto refuse = [this](const QString& message) -> std::optional<session::LaunchSpec> {
        fail(message);
        return std::nullopt;
    };
    const auto* harness = findHarness(request.harness);
    if (!harness)
        return refuse(QStringLiteral("Unknown agent harness."));
    const auto& directory = request.directory;
    if (directory.isEmpty() || directory.size() > 4096 || directory.contains(QChar::Null) ||
        directory.contains(QLatin1Char('\n')))
        return refuse(QStringLiteral("Choose a project directory (at most 4096 characters)."));
    if (!validModel(request.model) ||
        modelArguments(request.harness, request.model).isEmpty() != request.model.isEmpty())
        return refuse(QStringLiteral("This agent cannot take that model."));
    if (!request.mode.isEmpty() && modeArguments(request.harness, request.mode).isEmpty())
        return refuse(QStringLiteral("This agent has no such mode."));
    // The user's configured arguments, then this agent's model and mode.
    const auto arguments = defaultArguments(request.harness) +
                           harness_arguments_.value(request.harness) +
                           modelArguments(request.harness, request.model) +
                           modeArguments(request.harness, request.mode);
    if (!request.machine.isEmpty()) {
        std::optional<session::LaunchSpec> launch;
        const auto refusal =
            remoteLaunch(request, QString::fromLatin1(harness->command), arguments, launch);
        return refusal.isEmpty() ? launch : refuse(refusal);
    }
    const QString project =
        directory == QStringLiteral("~") || directory.startsWith(QStringLiteral("~/"))
            ? QDir::homePath() + directory.mid(1)
            : directory;
    if (!QFileInfo(project).isDir())
        return refuse(
            QStringLiteral("Choose an existing project directory (at most 4096 characters)."));
    const auto program = harnessExecutable(*harness);
    if (program.isEmpty())
        return refuse(QStringLiteral("%1 is not installed or is not available on PATH.")
                          .arg(QString::fromLatin1(harness->label)));
    // Codex and Claude Code have service-side observers; other CLIs run as
    // plain terminal agents.
    const auto mode = request.harness == QStringLiteral("codex")    ? session::AgentMode::codex
                      : request.harness == QStringLiteral("claude") ? session::AgentMode::claude
                                                                    : session::AgentMode::terminal;
    return session::validate_launch({program, arguments, project, {100, 30}, mode});
}
QString Workspace::startAgent(const AgentRequest& request) {
    if (!mutableRegistry())
        return {};
    if (preview_mode_)
        return failed(QStringLiteral("Agent launch is disabled in the preview fixture."));
    if (sessions_.size() >= 128 || !validName(request.title))
        return failed(QStringLiteral(
            "Use an agent name of 1–80 characters; at most 128 agents are supported."));
    if (std::none_of(categories_.begin(), categories_.end(),
                     [&](const auto& category) { return category.id == request.category; }))
        return failed(QStringLiteral("Unknown category."));
    try {
        const auto launch = agentLaunch(request);
        if (!launch)
            return {};
        return launchAgent(request, *launch);
    } catch (const std::exception& error) {
        return failed(QString::fromUtf8(error.what()));
    }
}
QString Workspace::launchAgent(const AgentRequest& request, const session::LaunchSpec& launch) {
    // An explicit attach has no registry to record the agent in.
    if (storage_path_.isEmpty())
        return failed(QStringLiteral("Agent launch requires a persisted workspace."));
    try {
        auto id = newId();
        const auto endpoint = session::posix::prepare_endpoint(
            QDir(QFileInfo(storage_path_).absolutePath()).filePath(id + QStringLiteral(".sock")));
        auto item =
            std::make_unique<SessionPreview>(request.title.trimmed(), launch.directory, QString{},
                                             QColor(QStringLiteral("#87cbac")), "");
        item->setSessionId(id);
        item->setHarnessId(request.harness);
        const Agent agent{request.category, endpoint, launch, request.harness};
        item->setStatusSource(statusSource(agent));
        const auto previous = checkpoint();
        agents_.insert(id, agent);
        sessions_.push_back(std::move(item));
        if (request.select)
            for (auto& category : categories_)
                if (category.id == request.category)
                    category.selected = id;
        restoreSelection();
        // Persist before starting a child, so a failed write cannot orphan a new agent.
        if (!save()) {
            sessions_.pop_back();
            rollback(previous);
            emit errorChanged();
            return {};
        }
        watch(sessions_.back().get());
        // Only this Mac's CLIs are updated first.
        if (!request.machine.isEmpty() || !deferForUpdate(id))
            sessions_.back()->startLive(endpoint, launch, session::wire::AttachMode::create);
        changed();
        return id;
    } catch (const std::exception& error) {
        return failed(QString::fromUtf8(error.what()));
    }
}
bool Workspace::save(const QString& renamedId, const QString& renamedTitle) {
    // Mutation callers publish failures only after restoring their previous state.
    const auto saveError = [this](const QString& message) {
        error_ = message;
        return false;
    };
    if (storage_path_.isEmpty())
        return true;
    if (storage_failed_)
        return saveError(
            QStringLiteral("Workspace registry could not be loaded; it has not been overwritten."));
    QJsonArray groups;
    for (const auto& category : categories_) {
        QJsonObject group{
            {"id", category.id}, {"name", category.name}, {"selected", category.selected}};
        if (!category.tiles.empty())
            group.insert(QStringLiteral("tiles"), category.tiles.toJson());
        groups.append(group);
    }
    QJsonArray agents;
    for (const auto& item : sessions_) {
        const auto entry = agents_.constFind(item->sessionId());
        if (entry == agents_.cend())
            return saveError(QStringLiteral("Missing agent metadata."));
        const auto& agent = entry.value();
        const auto resume = agent.launch.arguments.size() == 2 &&
                                    agent.launch.arguments.front() == QStringLiteral("resume")
                                ? agent.launch.arguments.at(1)
                                : QString{};
        auto serialized = QJsonObject{
            {"id", item->sessionId()},
            {"title", item->sessionId() == renamedId ? renamedTitle : item->title()},
            {"category", agent.category},
            {"endpoint", agent.endpoint},
            {"program", agent.launch.program},
            {"harness", agent.harness},
            {"mode", agent.launch.agent == session::AgentMode::claude ? QStringLiteral("claude")
                                                                      : QString()},
            {"resumeThread", resume},
            {"arguments", QJsonArray::fromStringList(agent.launch.arguments)},
            {"directory", agent.launch.directory}};
        const auto resume_option = resumeOption(agent.harness);
        if (agent.managed_resume_index >= 0 &&
            agent.managed_resume_index + 1 < agent.launch.arguments.size() &&
            !resume_option.isEmpty() &&
            agent.launch.arguments.at(agent.managed_resume_index) == resume_option &&
            agent.launch.arguments.at(agent.managed_resume_index + 1) ==
                agent.managed_resume_identity)
            serialized.insert(QStringLiteral("managedResume"),
                              QJsonObject{{"index", agent.managed_resume_index},
                                          {"identity", agent.managed_resume_identity}});
        agents.append(serialized);
    }
    QSaveFile file(storage_path_);
    if (!file.open(QIODevice::WriteOnly))
        return saveError(QStringLiteral("Cannot save workspace: ") + file.errorString());
    if (!file.setPermissions(QFile::ReadOwner | QFile::WriteOwner))
        return saveError(QStringLiteral("Cannot make workspace registry private: ") +
                         file.errorString());
    const auto data = QJsonDocument(QJsonObject{{"version", 2},
                                                {"activeCategory", active_category_},
                                                {"categories", groups},
                                                {"agents", agents}})
                          .toJson(QJsonDocument::Compact);
    if (data.size() > qint64{1024} * 1024)
        return saveError(QStringLiteral("Workspace metadata exceeds the 1 MiB limit."));
    if (file.write(data) != data.size() || !file.commit())
        return saveError(QStringLiteral("Cannot save workspace: ") + file.errorString());
    if (!error_.isEmpty())
        clearError();
    return true;
}
void Workspace::loadCategories(const QJsonArray& groups) {
    std::vector<Category> categories;
    QSet<QString> ids;
    for (const auto& value : groups) {
        const auto group = value.toObject();
        Category category{group.value(QStringLiteral("id")).toString(),
                          group.value(QStringLiteral("name")).toString(),
                          group.value(QStringLiteral("selected")).toString(),
                          {}};
        if (!validName(category.id) || !validName(category.name) || category.selected.size() > 80 ||
            ids.contains(category.id))
            throw std::runtime_error("Invalid or duplicate category");
        ids.insert(category.id);
        categories.push_back(category);
    }
    categories_ = std::move(categories);
}
namespace {
// The agent's own option (or Codex's subcommand) that takes a conversation to
// resume; harnesses whose option is unknown restart fresh in the same folder.
QString resumeOption(const QString& harness) {
    if (harness == QLatin1String("codex"))
        return QStringLiteral("resume");
    if (harness == QLatin1String("claude") || harness == QLatin1String("omp"))
        return QStringLiteral("--resume");
    if (harness == QLatin1String("grok"))
        return QStringLiteral("-r");
    if (harness == QLatin1String("opencode") || harness == QLatin1String("kimi"))
        return QStringLiteral("--session");
    if (harness == QLatin1String("agy"))
        return QStringLiteral("--conversation");
    return {};
}
// Services built before resume records keep none, and services built before
// the rollout scan do not follow /new. A Codex agent's app-server, found by the
// backend socket it listens on, still holds its threads' rollouts open.
std::vector<QString> openCodexThreads(const QString& endpoint) {
    // Only platform-owned process tools may attest an observer identity.
    const auto lsof = QStandardPaths::findExecutable(
        QStringLiteral("lsof"), {QStringLiteral("/usr/sbin"), QStringLiteral("/usr/bin")});
    const auto ps = QStandardPaths::findExecutable(
        QStringLiteral("ps"), {QStringLiteral("/bin"), QStringLiteral("/usr/bin")});
    if (lsof.isEmpty() || ps.isEmpty())
        return {};
    // No backend socket means no app-server can still hold a rollout open.
    if (!QFileInfo::exists(endpoint + QStringLiteral(".codex")))
        return {};
    QProcess list;
    list.start(ps, {QStringLiteral("-axo"), QStringLiteral("pid=,command=")});
    if (!list.waitForFinished(3000))
        return {};
    const auto listen =
        QStringLiteral("app-server --listen unix://") + endpoint + QStringLiteral(".codex");
    QString pid;
    for (const auto& line : QString::fromUtf8(list.readAllStandardOutput()).split('\n')) {
        const auto row = line.trimmed();
        if (row.endsWith(listen) || row.contains(listen + QLatin1Char(' '))) {
            if (!pid.isEmpty())
                return {};
            pid = row.section(QLatin1Char(' '), 0, 0);
        }
    }
    if (pid.isEmpty())
        return {};
    QProcess files;
    files.start(lsof, {QStringLiteral("-p"), pid, QStringLiteral("-Fn")});
    if (!files.waitForFinished(5000))
        return {};
    return session::codex_threads_from_open_files(QString::fromUtf8(files.readAllStandardOutput()));
}
// Hooks report a conversation at session start, before anything is saved; a
// conversation without a transcript cannot be resumed, so it starts fresh.
// CLIs whose storage is not known here are trusted to resume or say why not.
// Depth is fixed at three: YYYY/MM/DD, with no symlink traversal.
bool codexConversationSaved(const QDir& directory, const QString& conversation, int depth) {
    if (depth == 0) {
        QDirIterator rollouts(
            directory.path(),
            {QStringLiteral("rollout-*-") + conversation + QStringLiteral(".jsonl")},
            QDir::Files | QDir::NoSymLinks, QDirIterator::NoIteratorFlags);
        return rollouts.hasNext();
    }
    const auto pattern = QStringLiteral("[0-9]").repeated(depth == 3 ? 4 : 2);
    const auto folders =
        directory.entryList({pattern}, QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks,
                            QDir::Name | QDir::Reversed);
    return std::any_of(folders.begin(), folders.end(), [&](const QString& folder) {
        return codexConversationSaved(QDir(directory.filePath(folder)), conversation, depth - 1);
    });
}

// Whether a saved conversation resumes automatically. Codex and Claude report
// theirs to lapis's observer, which printed output cannot override; the other
// CLIs report theirs only through their session hook's terminal checkpoint.
// Records from before lapis recorded the source resume as they always did.
bool resumable(const session::ResumeRecord& record, const QString& harness) {
    if (record.source != session::ResumeSource::terminal)
        return true;
    return harness != QLatin1String("codex") && harness != QLatin1String("claude");
}

bool conversationSaved(const session::ResumeRecord& record) {
    const auto& conversation = record.session_id;
    if (record.agent == QLatin1String("claude")) {
        const QDir projects(qEnvironmentVariable("CLAUDE_CONFIG_DIR",
                                                 QDir::homePath() + QStringLiteral("/.claude")) +
                            QStringLiteral("/projects"));
        const auto transcript = conversation + QStringLiteral(".jsonl");
        const auto folders = projects.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        return std::any_of(folders.begin(), folders.end(), [&](const QString& folder) {
            return QFileInfo::exists(projects.filePath(folder + QLatin1Char('/') + transcript));
        });
    }
    if (record.agent == QLatin1String("codex")) {
        const QDir sessions(
            qEnvironmentVariable("CODEX_HOME", QDir::homePath() + QStringLiteral("/.codex")) +
            QStringLiteral("/sessions"));
        return codexConversationSaved(sessions, conversation, 3);
    }
    return true;
}
} // namespace
void Workspace::recordConversations() {
    QStringList endpoints;
    for (const auto& agent : std::as_const(agents_)) {
        if (agent.harness == QLatin1String("codex"))
            endpoints.append(agent.endpoint);
    }
    if (endpoints.isEmpty() || probing_->exchange(true))
        return;
    // Process listing is slow enough to keep off the GUI thread; the worker
    // touches only files beside each endpoint.
    QThreadPool::globalInstance()->start([endpoints, busy = probing_] {
        const auto done = qScopeGuard([busy] { busy->store(false); });
        for (const auto& endpoint : endpoints) {
            const auto open = openCodexThreads(endpoint);
            if (open.empty())
                continue;
            // A saved thread still loaded but no longer written last was left
            // by /new or /resume. One not loaded may be a thread the service
            // recorded before Codex wrote its rollout, so it stays.
            const auto saved = session::read_resume_record(endpoint);
            if (saved && saved->source == session::ResumeSource::observer &&
                (saved->session_id == open.front() ||
                 std::find(open.begin(), open.end(), saved->session_id) == open.end()))
                continue;
            try {
                session::write_resume_record(endpoint, {QStringLiteral("codex"), open.front(),
                                                        session::ResumeSource::observer});
            } catch (const std::exception& error) {
                qWarning().noquote() << "Resume record not saved:" << error.what();
            }
        }
    });
}
// Restart an ended or unreachable agent in its card, resuming its conversation.
bool Workspace::restartAgent(const QString& id) {
    if (!mutableRegistry())
        return false;
    if (preview_mode_ || storage_path_.isEmpty())
        return fail(QStringLiteral("Restart requires a persisted workspace."));
    const auto entry = agents_.find(id);
    auto* item = session(id);
    if (entry == agents_.end() || item == nullptr)
        return fail(QStringLiteral("Unknown agent."));
    if (item->closing())
        return fail(QStringLiteral("This agent is still closing."));
    if (serviceRunning(entry->endpoint))
        return fail(QStringLiteral("This agent is still running."));
    QString diagnostic;
    const auto launch = restoredLaunch(*entry, &diagnostic);
    if (!launch)
        return fail(diagnostic.isEmpty()
                        ? QStringLiteral("This agent's program or folder is no longer available.")
                        : diagnostic);
    const auto previous = checkpoint();
    entry->launch = launch->launch;
    entry->managed_resume_index = launch->managed_resume_index;
    entry->managed_resume_identity = launch->managed_resume_identity;
    if (!save()) {
        rollback(previous);
        emit errorChanged();
        return false;
    }
    item->startLive(entry->endpoint, entry->launch, session::wire::AttachMode::create);
    return true;
}
bool Workspace::serviceRunning(const QString& endpoint) {
    if (!QFileInfo::exists(endpoint))
        return false;
    QLocalSocket probe;
    probe.connectToServer(endpoint);
    if (probe.waitForConnected(250)) {
        probe.abort();
        return true;
    }
    // Only a refused or missing endpoint proves the service is gone.
    return probe.error() != QLocalSocket::ConnectionRefusedError &&
           probe.error() != QLocalSocket::ServerNotFoundError;
}
auto Workspace::restoredLaunch(const Agent& agent, QString* diagnostic)
    -> std::optional<ResumeLaunch> {
    auto launch = agent.launch;
    if (const auto* harness = findHarness(agent.harness);
        harness && !QFileInfo(launch.program).isExecutable())
        launch.program = harnessExecutable(*harness);
    if (launch.program.isEmpty() || !QFileInfo(launch.directory).isDir())
        return std::nullopt;
    const auto option = resumeOption(agent.harness);
    ResumeLaunch plan{std::move(launch), agent.managed_resume_index, agent.managed_resume_identity};
    const auto record = session::read_resume_record(agent.endpoint);
    if (record && !resumable(*record, agent.harness)) {
        // Retire only a proven lapis-owned pair. Explicit user arguments stay
        // authoritative, including when old derived metadata no longer matches.
        const auto index = plan.managed_resume_index;
        if (managedResumeMatches(plan.launch.arguments, index, option,
                                 plan.managed_resume_identity)) {
            plan.launch.arguments.remove(index, 2);
            plan.managed_resume_index = -1;
            plan.managed_resume_identity.clear();
        }
        qWarning().noquote()
            << "Automatic resume skipped: a printed checkpoint for a CLI lapis observes";
    }
    if (!option.isEmpty() && record && resumable(*record, agent.harness) &&
        record->agent == agent.harness && conversationSaved(*record)) {
        if (plan.managed_resume_index >= 0) {
            // Replace only the pair whose provenance the registry recorded.
            // A newer service checkpoint, including one after /clear, wins.
            if (managedResumeMatches(plan.launch.arguments, plan.managed_resume_index, option,
                                     plan.managed_resume_identity)) {
                plan.launch.arguments[plan.managed_resume_index + 1] = record->session_id;
                plan.managed_resume_identity = record->session_id;
            }
        } else if (std::none_of(plan.launch.arguments.cbegin(), plan.launch.arguments.cend(),
                                [&option](const QString& argument) {
                                    return argument == option ||
                                           (option.startsWith(QLatin1Char('-')) &&
                                            argument.startsWith(option + QLatin1Char('=')));
                                })) {
            // savedArguments() is the durable limit; an uncounted append must
            // not turn a loadable registry into one the next startup rejects.
            if (plan.launch.arguments.size() + 2 > max_saved_arguments) {
                qWarning().noquote() << "Resume record not added: saved launch already has"
                                     << plan.launch.arguments.size() << "arguments";
            } else {
                // No provenance means any matching argument is user-owned. Add a
                // managed pair only when the user supplied no such option at all.
                plan.managed_resume_index = static_cast<int>(plan.launch.arguments.size());
                plan.managed_resume_identity = record->session_id;
                plan.launch.arguments += QStringList{option, record->session_id};
            }
        }
    }
    try {
        plan.launch = session::validate_launch(plan.launch);
        return plan;
    } catch (const std::exception& error) {
        qWarning().noquote() << "Agent restart rejected:" << error.what();
        if (diagnostic)
            *diagnostic = QString::fromUtf8(error.what());
        return std::nullopt;
    }
}
QStringList Workspace::savedArguments(const QJsonValue& value) {
    const auto list = value.toArray();
    if (!value.isArray() || list.size() > max_saved_arguments)
        throw std::runtime_error("Invalid agent arguments");
    QStringList arguments;
    for (const auto& item : list) {
        const auto text = item.toString();
        if (!item.isString() || text.size() > 4096 || text.contains(QChar::Null))
            throw std::runtime_error("Invalid agent arguments");
        arguments.append(text);
    }
    return arguments;
}
void Workspace::loadManagedResume(const QJsonValue& value, Agent& agent) {
    const auto managed = value.toObject();
    const auto index = managed.value(QStringLiteral("index")).toInt(-1);
    const auto identity = managed.value(QStringLiteral("identity")).toString();
    const auto option = resumeOption(agent.harness);
    if (index < 0 || index >= agent.launch.arguments.size() - 1 ||
        !session::valid_resume_identity(identity) || option.isEmpty() ||
        agent.launch.arguments.at(index) != option ||
        agent.launch.arguments.at(index + 1) != identity) {
        // Provenance is derived state. A stale index/identity loses automatic
        // replacement only; it must not make the entire workspace unloadable.
        qWarning().noquote() << "Ignoring invalid managed resume provenance";
        return;
    }
    agent.managed_resume_index = index;
    agent.managed_resume_identity = identity;
}
void Workspace::loadAgents(const QJsonArray& agents) {
    QMap<QString, Agent> metadata;
    std::vector<std::unique_ptr<SessionPreview>> restored;
    const auto directory = QFileInfo(storage_path_).absolutePath();
    for (const auto& value : agents) {
        const auto object = value.toObject();
        const auto id = object.value(QStringLiteral("id")).toString();
        const auto title = object.value(QStringLiteral("title")).toString();
        const auto harness =
            object.value(QStringLiteral("harness")).toString(QStringLiteral("codex"));
        if (!findHarness(harness) || (object.contains(QStringLiteral("harness")) &&
                                      !object.value(QStringLiteral("harness")).isString()))
            throw std::runtime_error("Invalid agent harness");
        Agent agent{object.value(QStringLiteral("category")).toString(),
                    object.value(QStringLiteral("endpoint")).toString(),
                    {object.value(QStringLiteral("program")).toString(),
                     {},
                     object.value(QStringLiteral("directory")).toString(),
                     {100, 30},
                     harness == QStringLiteral("codex") ? session::AgentMode::codex
                                                        : session::AgentMode::terminal},
                    harness};
        const auto resume = object.value(QStringLiteral("resumeThread")).toString();
        if ((object.contains(QStringLiteral("resumeThread")) &&
             !object.value(QStringLiteral("resumeThread")).isString()) ||
            (!resume.isEmpty() && (harness != QStringLiteral("codex") || QUuid(resume).isNull())))
            throw std::runtime_error("Invalid native resume identity");
        if (!resume.isEmpty())
            agent.launch.arguments = {QStringLiteral("resume"), resume};
        // The full literal argument list, when recorded, is what the running
        // service was launched with; it is part of the launch fingerprint.
        if (object.contains(QStringLiteral("arguments")))
            agent.launch.arguments = savedArguments(object.value(QStringLiteral("arguments")));
        if (object.contains(QStringLiteral("managedResume")))
            loadManagedResume(object.value(QStringLiteral("managedResume")), agent);
        // Claude agents saved before the service adapter ran as terminals; the
        // launch must match the one their running service was created with.
        if (harness == QStringLiteral("claude") &&
            object.value(QStringLiteral("mode")).toString() == QStringLiteral("claude"))
            agent.launch.agent = session::AgentMode::claude;
        if (QUuid(id).isNull() || metadata.contains(id) || !validName(title) ||
            std::none_of(categories_.begin(), categories_.end(),
                         [&](const auto& category) { return category.id == agent.category; }) ||
            agent.endpoint != QDir(directory).filePath(id + QStringLiteral(".sock")) ||
            !QFileInfo(agent.launch.program).isAbsolute() || agent.launch.program.size() > 4096 ||
            agent.launch.program.contains(QChar::Null) ||
            !QFileInfo(agent.launch.directory).isAbsolute() ||
            agent.launch.directory.size() > 4096 || agent.launch.directory.contains(QChar::Null))
            throw std::runtime_error("Invalid agent record");
        auto item = std::make_unique<SessionPreview>(title, agent.launch.directory, QString{},
                                                     QColor(QStringLiteral("#87cbac")), "");
        item->setSessionId(id);
        item->setHarnessId(harness);
        item->setStatusSource(statusSource(agent));
        metadata.insert(id, agent);
        restored.push_back(std::move(item));
    }
    agents_ = std::move(metadata);
    sessions_ = std::move(restored);
}
void Workspace::lockRegistry() {
    registry_lock_ = std::make_unique<QLockFile>(storage_path_ + QStringLiteral(".lock"));
    // This lock lasts for the window lifetime; elapsed time cannot steal it.
    registry_lock_->setStaleLockTime(0);
    QFile marker(storage_path_ + QStringLiteral(".restoring"));
    if (!registry_lock_->tryLock(0)) {
        // The login helper holds the workspace only while it restarts agents
        // and names itself in a marker holding its process ID; a window opened
        // meanwhile waits for it.
        qint64 holder{};
        QString host;
        QString name;
        const bool helper = registry_lock_->getLockInfo(&holder, &host, &name) &&
                            marker.open(QIODevice::ReadOnly) &&
                            marker.read(32).trimmed().toLongLong() == holder;
        marker.close();
        if (!helper || headless_)
            throw std::runtime_error("This workspace is already open in another lapis window");
        // A windowless host keeps serving until asked; the login helper exits
        // once its agents answer. Either way, wait at most two minutes.
        QElapsedTimer waited;
        waited.start();
        while (!registry_lock_->tryLock(500)) {
            if (waited.elapsed() >= 120000)
                throw std::runtime_error("This workspace is already open in another lapis window");
            WorkspaceControl::requestHandover(storage_path_);
        }
    }
    if (headless_ && marker.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        marker.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
        marker.write(QByteArray::number(QCoreApplication::applicationPid()));
    }
}
void Workspace::restore() {
    try {
        // The endpoint helper validates private ownership and every ancestor.
        const auto validated =
            session::posix::prepare_endpoint(QDir(QFileInfo(storage_path_).absolutePath())
                                                 .filePath(QStringLiteral("registry-check.sock")));
        storage_path_ =
            QDir(QFileInfo(validated).absolutePath()).filePath(QFileInfo(storage_path_).fileName());
        lockRegistry();
        if (QFileInfo(storage_path_).isSymLink())
            throw std::runtime_error("Workspace registry cannot be a symlink");
        QFile file(storage_path_);
        if (!file.exists())
            return;
        if (!file.open(QIODevice::ReadOnly) || file.size() > qint64{1024} * 1024)
            throw std::runtime_error("Workspace registry is unreadable or too large");
        QJsonParseError parse;
        const auto document = QJsonDocument::fromJson(file.readAll(), &parse);
        const auto root = document.object();
        const auto groups = root.value(QStringLiteral("categories")).toArray();
        const auto agents = root.value(QStringLiteral("agents")).toArray();
        if (parse.error != QJsonParseError::NoError ||
            (root.value(QStringLiteral("version")).toInt() != 1 &&
             root.value(QStringLiteral("version")).toInt() != 2) ||
            !root.value(QStringLiteral("agents")).isArray() || groups.isEmpty() ||
            groups.size() > 32 || agents.size() > 128)
            throw std::runtime_error("Invalid workspace registry format");
        loadCategories(groups);
        loadAgents(agents);
        loadTiles(groups);
        active_category_ = root.value(QStringLiteral("activeCategory")).toString();
        if (std::none_of(categories_.begin(), categories_.end(),
                         [&](const auto& category) { return category.id == active_category_; }))
            active_category_ = categories_.front().id;
        bool restarted = false;
        for (const auto& item : sessions_) {
            watch(item.get());
            const auto entry = agents_.find(item->sessionId());
            if (entry == agents_.end())
                throw std::runtime_error("Missing restored agent metadata");
            auto& agent = entry.value();
            // A card still in the workspace whose service is gone comes back,
            // like a restored terminal tab; Command-W is what removes an agent.
            if (restore_agents_ && !serviceRunning(agent.endpoint))
                if (const auto launch = restoredLaunch(agent)) {
                    agent.launch = launch->launch;
                    agent.managed_resume_index = launch->managed_resume_index;
                    agent.managed_resume_identity = launch->managed_resume_identity;
                    item->startLive(agent.endpoint, agent.launch,
                                    session::wire::AttachMode::create);
                    restarted = true;
                    continue;
                }
            if (headless_)
                continue; // running services are the window's to reattach
            item->startLive(agent.endpoint, agent.launch, session::wire::AttachMode::reconnect);
        }
        // Restarted agents have new launch arguments, part of their fingerprint.
        if (restarted && !save())
            throw std::runtime_error(error_.toStdString());
        if (restore_agents_ && !headless_) {
            conversation_timer_.setInterval(60000);
            connect(&conversation_timer_, &QTimer::timeout, this, &Workspace::recordConversations);
            conversation_timer_.start();
            recordConversations();
        }
    } catch (const std::exception& error) {
        sessions_.clear();
        agents_.clear();
        categories_ = {{QStringLiteral("general"), QStringLiteral("General"), {}, {}}};
        active_category_ = QStringLiteral("general");
        focused_index_ = -1;
        storage_failed_ = true;
        fail(QStringLiteral("Cannot restore workspace: ") + QString::fromUtf8(error.what()));
    }
}
void SessionPreview::setUpdating(const QString& label) {
    if (updating_ == label)
        return;
    updating_ = label;
    emit statusChanged();
}

bool Workspace::deferForUpdate(const QString& id) {
    const auto entry = agents_.constFind(id);
    if (entry == agents_.constEnd())
        return false;
    const auto harness = entry->harness;
    const auto program = entry->launch.program;
    const auto* selected = findHarness(harness);
    if (!update_harnesses_ || !selected || !selected->update)
        return false;
    const bool running = harness_updates_.value(harness) != nullptr;
    constexpr qint64 fresh_ms = qint64{30} * 60 * 1000;
    if (!running &&
        QDateTime::currentMSecsSinceEpoch() - harness_checked_ms_.value(harness, 0) < fresh_ms)
        return false;
    starts_after_update_[harness].append(id);
    if (auto* item = session(id))
        item->setUpdating(QStringLiteral("Updating %1…").arg(QLatin1String(selected->label)));
    if (running)
        return true;
    auto* process = new QProcess(this);
    harness_updates_.insert(harness, process);
    process->setProgram(program);
    process->setArguments({QString::fromLatin1(selected->update)});
    process->setStandardInputFile(QProcess::nullDevice());
    process->setProcessChannelMode(QProcess::MergedChannels);
    connect(process, &QProcess::finished, this,
            [this, harness, process](int code, QProcess::ExitStatus status) {
                finishUpdate(harness, process,
                             status == QProcess::NormalExit ? QStringLiteral("exit %1").arg(code)
                                                            : QStringLiteral("crashed"));
            });
    connect(process, &QProcess::errorOccurred, this,
            [this, harness, process](QProcess::ProcessError error) {
                if (error == QProcess::FailedToStart)
                    finishUpdate(harness, process, QStringLiteral("could not start"));
            });
    // A stuck update must not keep the agent from starting.
    QTimer::singleShot(120000, process, [this, harness, process] {
        if (process->state() == QProcess::NotRunning)
            return;
        process->kill();
        finishUpdate(harness, process, QStringLiteral("stopped after 2 minutes"));
    });
    process->start();
    return true;
}

void Workspace::finishUpdate(const QString& harness, QProcess* process, const QString& outcome) {
    if (harness_updates_.value(harness) != process)
        return;
    harness_updates_.remove(harness);
    harness_checked_ms_.insert(harness, QDateTime::currentMSecsSinceEpoch());
    const auto output = QString::fromUtf8(process->readAll()).simplified().right(600);
    logUpdate(
        QStringLiteral("%1 %2 update: %3. %4")
            .arg(QDateTime::currentDateTime().toString(Qt::ISODate), harness, outcome, output));
    process->deleteLater();
    for (const auto& id : starts_after_update_.take(harness)) {
        auto* item = session(id);
        const auto entry = agents_.constFind(id);
        if (!item || entry == agents_.constEnd())
            continue; // closed while waiting
        item->setUpdating({});
        item->startLive(entry->endpoint, entry->launch, session::wire::AttachMode::create);
    }
}

// Beside the registry; the previous log is kept once it passes 256 KiB.
void Workspace::logUpdate(const QString& line) const {
    const auto path = QDir(QFileInfo(storage_path_).absolutePath())
                          .filePath(QStringLiteral("harness-updates.log"));
    if (QFileInfo(path).size() > qint64{256} * 1024) {
        QFile::remove(path + QStringLiteral(".1"));
        QFile::rename(path, path + QStringLiteral(".1"));
    }
    QFile file(path);
    if (file.open(QIODevice::Append | QIODevice::Text)) {
        file.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
        file.write(line.toUtf8() + '\n');
    }
}

void SessionPreview::setClosing(bool closing) {
    if (closing_ == closing)
        return;
    closing_ = closing;
    emit statusChanged();
}

bool Workspace::closeSession(const QString& id, bool abandon) {
    if (!mutableRegistry())
        return false;
    auto* item = session(id);
    if (!item)
        return false;
    if (!item->live() || item->connectionState() == QStringLiteral("ended") ||
        (abandon && !item->reachable()))
        return discardSession(id);
    if (item->closing())
        return true;
    if (!item->terminate())
        return fail(QStringLiteral("lapis could not reach this agent's session to end it. "
                                   "Reconnect it, or exit the agent, then close its tab."));
    item->setClosing(true);
    return true;
}

void Workspace::finishClosing(SessionPreview* item) {
    if (!item->closing())
        return;
    const auto state = item->connectionState();
    if (state == QStringLiteral("ended")) {
        const QPointer<SessionPreview> guarded(item);
        // Remove outside the connection's own signal delivery.
        QMetaObject::invokeMethod(
            this,
            [this, guarded] {
                if (guarded && guarded->closing() && removeSession(guarded->sessionId()))
                    return;
                if (guarded)
                    guarded->setClosing(false);
            },
            Qt::QueuedConnection);
        return;
    }
    if (state == QStringLiteral("disconnected") || state == QStringLiteral("replaced")) {
        // A session service older than the close request rejects it and drops
        // this view while its agent keeps running: reattach and say so.
        item->setClosing(false);
        fail(QStringLiteral("This agent's session service predates closing from lapis, so the "
                            "agent is still running. Exit it inside the agent, then close its "
                            "tab."));
        item->reconnect();
    }
}
void SessionPreview::setUnseen(bool unseen) {
    if (unseen_ == unseen)
        return;
    unseen_ = unseen;
    emit unseenChanged();
}

void SessionPreview::noteOutput() {
    if (status_source_ != StatusSource::output)
        return;
    const auto now = QDateTime::currentMSecsSinceEpoch();
    // Reattaching replays the screen; that is not the agent working.
    if (ready_since_ == 0 || now - ready_since_ < output_timing_.settle_ms)
        return;
    output_times_.push_back(now);
    if (output_times_.size() > 3)
        output_times_.erase(output_times_.begin());
    if (!output_active_ && output_times_.size() == 3 &&
        now - output_times_.front() <= output_timing_.burst_ms) {
        output_active_ = true;
        output_quiet_ = false;
        emit statusChanged();
    }
    if (output_active_)
        quiet_timer_.start(output_timing_.quiet_ms);
}

// A turn ending marks an unselected agent; new requests are marked where the
// request count changes.
void Workspace::noteStatus(SessionPreview* item) {
    const auto now = item->statusKind();
    const auto previous = last_kind_.value(item);
    last_kind_.insert(item, now);
    if (previous == now)
        return;
    const bool finished = previous == QStringLiteral("working") &&
                          (now == QStringLiteral("finished") || now == QStringLiteral("idle"));
    if (finished && item->statusSource() != SessionPreview::StatusSource::output)
        emit turnFinished(item);
    if (finished && item != focusedSession())
        item->setUnseen(true);
}
SessionPreview::StatusSource Workspace::statusSource(const Agent& agent) {
    return agent.launch.agent == session::AgentMode::terminal
               ? SessionPreview::StatusSource::output
               : SessionPreview::StatusSource::observer;
}
// Command-Shift-T brings it back, resuming its conversation where it can.
void Workspace::rememberClosed(const Agent& agent, const QString& title) {
    if (preview_mode_)
        return;
    auto plan = restoredLaunch(agent);
    if (!plan)
        return;
    closed_.push_back({agent.category, title, agent.harness, std::move(*plan),
                       QFileInfo(agent.launch.program).fileName() == QStringLiteral("ssh")});
    if (closed_.size() > 10)
        closed_.erase(closed_.begin());
    emit closedChanged();
}
bool Workspace::reopenAgent() {
    if (closed_.empty() || !mutableRegistry())
        return false;
    auto closed = std::move(closed_.back());
    closed_.pop_back();
    emit closedChanged();
    const auto target = category(closed.category) != nullptr ? closed.category : active_category_;
    const AgentRequest request{.category = target,
                               .directory = closed.plan.launch.directory,
                               .title = closed.title,
                               .harness = closed.harness,
                               .machine = closed.remote ? QStringLiteral("ssh") : QString(),
                               .program = closed.plan.launch.program,
                               .model = {},
                               .mode = {},
                               .select = true};
    const auto id = launchAgent(request, closed.plan.launch);
    if (id.isEmpty())
        return false;
    // The resume pair lapis added stays lapis's to update on later restarts.
    auto& agent = agents_[id];
    agent.managed_resume_index = closed.plan.managed_resume_index;
    agent.managed_resume_identity = closed.plan.managed_resume_identity;
    static_cast<void>(save());
    return selectSession(id);
}

// Tiles -----------------------------------------------------------------------

Workspace::Category* Workspace::category(const QString& id) {
    const auto found = std::find_if(categories_.begin(), categories_.end(),
                                    [&](const auto& value) { return value.id == id; });
    return found == categories_.end() ? nullptr : &*found;
}
const Workspace::Category* Workspace::activeCategory() const {
    const auto found = std::find_if(categories_.begin(), categories_.end(), [&](const auto& value) {
        return value.id == active_category_;
    });
    return found == categories_.end() ? nullptr : &*found;
}
void Workspace::untile(Category& category, const QString& id) {
    category.tiles.remove(id);
    if (category.tiles.count() < 2)
        category.tiles = {};
}
void Workspace::loadTiles(const QJsonArray& groups) {
    for (const auto& value : groups) {
        const auto group = value.toObject();
        auto* place = category(group.value(QStringLiteral("id")).toString());
        if (place == nullptr)
            continue;
        QSet<QString> members;
        for (auto agent = agents_.cbegin(); agent != agents_.cend(); ++agent)
            if (agent->category == place->id)
                members.insert(agent.key());
        place->tiles =
            TileLayout::fromJson(group.value(QStringLiteral("tiles")).toObject(), members);
        if (place->tiles.count() < 2)
            place->tiles = {};
    }
}
QVariantList Workspace::stageTiles() const {
    QVariantList result;
    const auto* active = activeCategory();
    if (active == nullptr || active->tiles.empty())
        return result;
    for (const auto& tile : active->tiles.tiles(QRectF(0, 0, 1, 1))) {
        auto* item = session(tile.session);
        if (item == nullptr)
            continue;
        result.append(QVariantMap{{QStringLiteral("sessionId"), tile.session},
                                  {QStringLiteral("session"), QVariant::fromValue(item)},
                                  {QStringLiteral("x"), tile.rect.x()},
                                  {QStringLiteral("y"), tile.rect.y()},
                                  {QStringLiteral("width"), tile.rect.width()},
                                  {QStringLiteral("height"), tile.rect.height()}});
    }
    return result;
}
QVariantList Workspace::stageDividers() const {
    QVariantList result;
    const auto* active = activeCategory();
    if (active == nullptr || active->tiles.empty())
        return result;
    for (const auto& divider : active->tiles.dividers(QRectF(0, 0, 1, 1)))
        result.append(QVariantMap{{QStringLiteral("path"), divider.path},
                                  {QStringLiteral("stacked"), divider.stacked},
                                  {QStringLiteral("x"), divider.line.x()},
                                  {QStringLiteral("y"), divider.line.y()},
                                  {QStringLiteral("width"), divider.line.width()},
                                  {QStringLiteral("height"), divider.line.height()},
                                  {QStringLiteral("areaX"), divider.area.x()},
                                  {QStringLiteral("areaY"), divider.area.y()},
                                  {QStringLiteral("areaWidth"), divider.area.width()},
                                  {QStringLiteral("areaHeight"), divider.area.height()}});
    return result;
}
// QML positional API v1 requires QString arguments; role names and boundary validation are
// explicit. NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
bool Workspace::tileSession(const QString& id, const QString& target, const QString& edge) {
    if (!mutableRegistry())
        return false;
    const auto side = TileLayout::edge(edge);
    auto* place = category(agents_.value(id).category);
    if (!session(id) || !side || place == nullptr)
        return fail(QStringLiteral("Agent no longer exists."));
    const QString beside = target.isEmpty() ? place->selected : target;
    if (beside.isEmpty() || beside == id || agents_.value(beside).category != place->id)
        return false;
    const auto previous = checkpoint();
    auto tiles = place->tiles;
    // The first split starts from the agent the stage shows.
    if (tiles.empty())
        tiles.place(beside, {}, TileLayout::Edge::center);
    if (!tiles.place(id, beside, *side))
        return fail(
            QStringLiteral("The stage holds at most %1 agents.").arg(TileLayout::kMaximumTiles));
    place->tiles = tiles.count() < 2 ? TileLayout{} : tiles;
    place->selected = id;
    active_category_ = place->id;
    if (!commit(previous))
        return false;
    changed();
    return true;
}
bool Workspace::untileSession(const QString& id) {
    if (!mutableRegistry())
        return false;
    auto* place = category(agents_.value(id).category);
    if (place == nullptr || !place->tiles.contains(id))
        return false;
    const auto previous = checkpoint();
    untile(*place, id);
    if (!commit(previous))
        return false;
    changed();
    return true;
}
bool Workspace::setTileRatio(const QString& path, qreal ratio, bool persist) {
    if (!mutableRegistry())
        return false;
    auto* place = category(active_category_);
    if (place == nullptr)
        return false;
    const auto previous = checkpoint();
    if (!place->tiles.setRatio(path, ratio))
        return false;
    // A drag moves the divider every frame; the registry is written when it ends.
    if (persist && !commit(previous))
        return false;
    emit tilesChanged();
    return true;
}
bool Workspace::focusTile(const QString& direction) {
    const auto* active = activeCategory();
    const auto side = TileLayout::edge(direction);
    if (active == nullptr || !side || active->tiles.empty())
        return false;
    const auto next = active->tiles.neighbor(active->selected, *side);
    return !next.isEmpty() && selectSession(next);
}
QString Workspace::splitAgent(const QString& edge) {
    if (!mutableRegistry())
        return {};
    const auto* item = focusedSession();
    if (item == nullptr || !TileLayout::edge(edge))
        return {};
    const auto entry = agents_.constFind(item->sessionId());
    if (entry == agents_.cend())
        return failed(QStringLiteral("Missing agent metadata."));
    // The same CLI, folder, model and mode, as a new conversation.
    auto launch = entry->launch;
    if (entry->managed_resume_index >= 0 &&
        entry->managed_resume_index + 1 < launch.arguments.size())
        launch.arguments.remove(entry->managed_resume_index, 2);
    else if (launch.arguments.size() == 2 && launch.arguments.front() == QStringLiteral("resume"))
        launch.arguments.clear();
    const bool remote = QFileInfo(launch.program).fileName() == QStringLiteral("ssh");
    AgentRequest request{.category = entry->category,
                         .directory = launch.directory,
                         .title = item->title(),
                         .harness = entry->harness,
                         .machine = remote ? QStringLiteral("ssh") : QString(),
                         .program = launch.program,
                         .model = {},
                         .mode = {},
                         .select = false};
    if (preview_mode_)
        return failed(QStringLiteral("Agent launch is disabled in the preview fixture."));
    const auto beside = item->sessionId();
    const auto id = launchAgent(request, launch);
    if (!id.isEmpty())
        tileSession(id, beside, edge);
    return id;
}
} // namespace lapis::desktop
