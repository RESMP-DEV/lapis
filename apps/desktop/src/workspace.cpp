#include "workspace.hpp"
#include "live_connection.hpp"
#include "platform/posix/local_endpoint.hpp"

#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>
#include <algorithm>
#include <array>
#include <iterator>
#include <stdexcept>
#include <utility>

#include <QDir>
#include <QFileInfo>

namespace lapis::desktop {
namespace {
struct Harness {
    const char* id;
    const char* label;
    const char* command;
};
constexpr std::array harness_catalog{
    Harness{"codex", "Codex", "codex"},    Harness{"claude", "Claude", "claude"},
    Harness{"omp", "OMP", "omp"},          Harness{"grok", "Grok", "grok"},
    Harness{"kimi", "Kimi", "kimi"},       Harness{"opencode", "OpenCode", "opencode"},
    Harness{"gemini", "Gemini", "gemini"}, Harness{"agy", "Antigravity", "agy"}};

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
constexpr std::string_view kPreviewPalette =
    "\x1b]10;rgb:d9/de/e8\x1b\\\x1b]11;rgb:0d/13/1d\x1b\\"
    "\x1b]4;2;rgb:87/cb/ac\x1b\\\x1b]4;4;rgb:9c/b4/ee\x1b\\"
    "\x1b]4;3;rgb:df/bb/7b\x1b\\\x1b]4;5;rgb:ba/a4/e8\x1b\\"
    "\x1b]4;8;rgb:75/83/98\x1b\\";
} // namespace

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

QString Workspace::rootDirectory() {
    return QFileInfo{QStringLiteral(LAPIS_PROJECT_ROOT)}.absoluteFilePath();
}

QString Workspace::defaultEndpoint() {
    return QDir{rootDirectory()}.filePath(QStringLiteral("runtime/desktop-v6.sock"));
}

Workspace::Workspace(WorkspaceMode mode, WorkspaceOptions options)
    : preview_mode_(mode == WorkspaceMode::preview) {
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
    categories_.push_back({QStringLiteral("general"), QStringLiteral("General"), {}});
    if (!preview_mode_) {
        if (options.launch || !options.endpoint.isEmpty()) {
            const auto launch = options.launch ? session::validate_launch(*options.launch)
                                               : session::shell_launch(rootDirectory());
            add("Agent", launch.directory.toUtf8().constData(), "Connecting", "#87cbac", "");
            sessions_.front()->setSessionId(QStringLiteral("shell"));
            agents_.insert(QStringLiteral("shell"), {active_category_, options.endpoint, launch});
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
bool Workspace::addCategory(const QString& name) {
    if (!mutableRegistry())
        return false;
    if (!validName(name) || categories_.size() >= 32)
        return fail(QStringLiteral(
            "Use a category name of 1–80 characters; at most 32 categories are supported."));
    const auto previous = checkpoint();
    categories_.push_back({newId(), name.trimmed(), {}});
    active_category_ = categories_.back().id;
    if (!commit(previous))
        return false;
    changed();
    return true;
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
    for (auto& category : categories_)
        if (category.id == active_category_)
            category.selected = id;
    if (!commit(previous))
        return false;
    if (category_changed) {
        emit categoryChanged();
        emit sessionsChanged();
    }
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
    entry->category = categoryId;
    for (auto& category : categories_)
        if (category.selected == id)
            category.selected.clear();
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
    const auto previous = checkpoint();
    // A closed agent hands focus to its right neighbor, or its left one at the end.
    const auto list = categorySessions();
    for (qsizetype i = 0; i < list.size(); ++i) {
        if (list[i].value<SessionPreview*>() != item)
            continue;
        const auto next =
            list.size() < 2
                ? QString()
                : list[i + 1 < list.size() ? i + 1 : i - 1].value<SessionPreview*>()->sessionId();
        for (auto& category : categories_)
            if (category.selected == id)
                category.selected = next;
    }
    last_kind_.remove(item);
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
    changed();
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
QVariantList Workspace::availableHarnesses() const {
    QVariantList result;
    for (const auto& harness : harness_catalog) {
        const auto program = harnessExecutable(harness);
        result.append(QVariantMap{{QStringLiteral("id"), QString::fromLatin1(harness.id)},
                                  {QStringLiteral("name"), QString::fromLatin1(harness.label)},
                                  {QStringLiteral("installed"), !program.isEmpty()}});
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
bool Workspace::createAgent(const QString& directory, const QString& title,
                            const QString& harness) {
    if (!mutableRegistry())
        return false;
    if (preview_mode_)
        return fail(QStringLiteral("Agent launch is disabled in the preview fixture."));
    if (sessions_.size() >= 128 || !validName(title))
        return fail(QStringLiteral(
            "Use an agent name of 1–80 characters; at most 128 agents are supported."));
    try {
        const QString project =
            directory == QStringLiteral("~") || directory.startsWith(QStringLiteral("~/"))
                ? QDir::homePath() + directory.mid(1)
                : directory;
        if (project.isEmpty() || project.size() > 4096 || project.contains(QChar::Null) ||
            !QFileInfo(project).isDir())
            return fail(
                QStringLiteral("Choose an existing project directory (at most 4096 characters)."));
        const auto* selected = findHarness(harness);
        if (!selected)
            return fail(QStringLiteral("Unknown agent harness."));
        const auto program = harnessExecutable(*selected);
        if (program.isEmpty())
            return fail(QStringLiteral("%1 is not installed or is not available on PATH.")
                            .arg(QString::fromLatin1(selected->label)));
        // Codex and Claude Code have service-side observers; other CLIs run as
        // plain terminal agents.
        const auto mode = harness == QStringLiteral("codex")    ? session::AgentMode::codex
                          : harness == QStringLiteral("claude") ? session::AgentMode::claude
                                                                : session::AgentMode::terminal;
        const auto id = newId();
        const auto endpoint = session::posix::prepare_endpoint(
            QDir(QFileInfo(storage_path_).absolutePath()).filePath(id + QStringLiteral(".sock")));
        auto launch = session::validate_launch(
            {program, harness_arguments_.value(harness), project, {100, 30}, mode});
        auto item = std::make_unique<SessionPreview>(title.trimmed(), launch.directory, QString{},
                                                     QColor(QStringLiteral("#87cbac")), "");
        item->setSessionId(id);
        item->setHarnessId(harness);
        const Agent agent{active_category_, endpoint, launch, harness};
        item->setStatusSource(statusSource(agent));
        const auto previous = checkpoint();
        agents_.insert(id, agent);
        sessions_.push_back(std::move(item));
        for (auto& category : categories_)
            if (category.id == active_category_)
                category.selected = id;
        restoreSelection();
        // Persist before starting a child, so a failed write cannot orphan a new agent.
        if (!save()) {
            sessions_.pop_back();
            rollback(previous);
            emit errorChanged();
            return false;
        }
        watch(sessions_.back().get());
        sessions_.back()->startLive(endpoint, launch, session::wire::AttachMode::create);
        changed();
        return true;
    } catch (const std::exception& error) {
        return fail(QString::fromUtf8(error.what()));
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
    for (const auto& category : categories_)
        groups.append(QJsonObject{
            {"id", category.id}, {"name", category.name}, {"selected", category.selected}});
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
        agents.append(QJsonObject{
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
            {"directory", agent.launch.directory}});
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
                          group.value(QStringLiteral("selected")).toString()};
        if (!validName(category.id) || !validName(category.name) || category.selected.size() > 80 ||
            ids.contains(category.id))
            throw std::runtime_error("Invalid or duplicate category");
        ids.insert(category.id);
        categories.push_back(category);
    }
    categories_ = std::move(categories);
}
QStringList Workspace::savedArguments(const QJsonValue& value) {
    const auto list = value.toArray();
    if (!value.isArray() || list.size() > 64)
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
void Workspace::restore() {
    try {
        // The endpoint helper validates private ownership and every ancestor.
        const auto validated =
            session::posix::prepare_endpoint(QDir(QFileInfo(storage_path_).absolutePath())
                                                 .filePath(QStringLiteral("registry-check.sock")));
        storage_path_ =
            QDir(QFileInfo(validated).absolutePath()).filePath(QFileInfo(storage_path_).fileName());
        registry_lock_ = std::make_unique<QLockFile>(storage_path_ + QStringLiteral(".lock"));
        // This lock lasts for the window lifetime; elapsed time cannot steal it.
        registry_lock_->setStaleLockTime(0);
        if (!registry_lock_->tryLock(0))
            throw std::runtime_error("This workspace is already open in another lapis window");
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
        active_category_ = root.value(QStringLiteral("activeCategory")).toString();
        if (std::none_of(categories_.begin(), categories_.end(),
                         [&](const auto& category) { return category.id == active_category_; }))
            active_category_ = categories_.front().id;
        for (const auto& item : sessions_) {
            watch(item.get());
            const auto entry = agents_.constFind(item->sessionId());
            if (entry == agents_.cend())
                throw std::runtime_error("Missing restored agent metadata");
            const auto& agent = entry.value();
            item->startLive(agent.endpoint, agent.launch, session::wire::AttachMode::reconnect);
        }
    } catch (const std::exception& error) {
        sessions_.clear();
        agents_.clear();
        categories_ = {{QStringLiteral("general"), QStringLiteral("General"), {}}};
        active_category_ = QStringLiteral("general");
        focused_index_ = -1;
        storage_failed_ = true;
        fail(QStringLiteral("Cannot restore workspace: ") + QString::fromUtf8(error.what()));
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
        (abandon && !item->inputReady()))
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
    if (previous == now || item == focusedSession())
        return;
    const bool finished = previous == QStringLiteral("working") &&
                          (now == QStringLiteral("finished") || now == QStringLiteral("idle"));
    if (finished)
        item->setUnseen(true);
}
SessionPreview::StatusSource Workspace::statusSource(const Agent& agent) {
    return agent.launch.agent == session::AgentMode::terminal
               ? SessionPreview::StatusSource::output
               : SessionPreview::StatusSource::observer;
}
} // namespace lapis::desktop
