#include "workspace.hpp"
#include "live_connection.hpp"
#include "platform/posix/local_endpoint.hpp"

#include <QFutureWatcher>
#include <QPromise>
#include <QStandardPaths>
#include <QThreadPool>
#include <QTimer>
#include <algorithm>
#include <array>
#include <stdexcept>
#include <utility>

#include <QDir>
#include <QFileInfo>

namespace lapis::desktop {
namespace {
constexpr std::string_view kPreviewPalette =
    "\x1b]10;rgb:d9/de/e8\x1b\\\x1b]11;rgb:0d/13/1d\x1b\\"
    "\x1b]4;2;rgb:87/cb/ac\x1b\\\x1b]4;4;rgb:9c/b4/ee\x1b\\"
    "\x1b]4;3;rgb:df/bb/7b\x1b\\\x1b]4;5;rgb:ba/a4/e8\x1b\\"
    "\x1b]4;8;rgb:75/83/98\x1b\\";
}

namespace {
struct LoadedWorkspace {
    std::shared_ptr<WorkspaceRegistry> registry;
    std::vector<WorkspaceEntry> entries;
    QString error;
};
template <typename Result, typename Work> QFuture<Result> background(Work work) {
    QPromise<Result> promise;
    auto future = promise.future();
    QThreadPool::globalInstance()->start(
        [promise = std::move(promise), work = std::move(work)]() mutable {
            promise.start();
            promise.addResult(work());
            promise.finish();
        });
    return future;
}
} // namespace

class Workspace::Storage {
  public:
    std::shared_ptr<WorkspaceRegistry> registry;
    QFutureWatcher<LoadedWorkspace> load;
    QFutureWatcher<QString> save;
    std::vector<WorkspaceEntry> written;
    bool writing{};
    bool dirty{};
};

Workspace::~Workspace() try {
    if (!registry_ready_ || !storage_ || !storage_->writing || !storage_->dirty)
        return;
    std::vector<WorkspaceEntry> entries;
    for (const auto& document : sessions_)
        if (const auto entry = document->reconnectEntry())
            entries.push_back(*entry);
    // The final coalesced snapshot must survive destruction of the GUI watcher.
    // Chain after the in-flight write; filesystem work stays off the GUI thread.
    static_cast<void>(storage_->save.future().then(
        QThreadPool::globalInstance(),
        [registry = storage_->registry, entries](const QString& previous_error) {
            if (!previous_error.isEmpty())
                return;
            try {
                registry->write(entries);
            } catch (const std::exception& error) {
                qWarning("Final workspace save failed: %s", error.what());
            }
        }));
} catch (const std::exception& error) {
    qWarning("Could not queue final workspace save: %s", error.what());
} catch (...) {
    qWarning("Could not queue final workspace save");
}

bool Workspace::canAddSessions() const {
    return registry_ready_ && sessions_.size() < WorkspaceRegistry::maximum_entries;
}

bool Workspace::canRetrySave() const {
    return !registry_ready_ && storage_ && storage_->registry && !storage_->writing;
}

void Workspace::retrySave() {
    if (canRetrySave())
        persistRegistry(true);
}

void Workspace::loadRegistry() {
    storage_ = std::make_unique<Storage>();
    connect(&storage_->load, &QFutureWatcher<LoadedWorkspace>::finished, this, [this] {
        const auto loaded = storage_->load.result();
        loading_ = false;
        if (!loaded.error.isEmpty()) {
            status_ = loaded.error;
            emit workspaceChanged();
            return;
        }
        storage_->registry = loaded.registry;
        storage_->written = loaded.entries;
        for (const auto& entry : loaded.entries) {
            auto item = std::make_unique<SessionPreview>(
                entry.title, entry.directory, QStringLiteral("Connecting"), QColor{"#87cbac"}, "");
            item->setSessionId(QString::fromLatin1(entry.identity.session_id.toHex()));
            auto* document = item.get();
            appendSession(std::move(item));
            document->restoreLive(entry);
        }
        registry_ready_ = true;
        status_ = sessions_.empty() ? QStringLiteral("Create a session to begin.") : QString{};
        emit sessionsChanged();
        emit focusChanged();
        emit workspaceChanged();
    });
    storage_->load.setFuture(background<LoadedWorkspace>([path = manifest_] {
        LoadedWorkspace result;
        try {
            result.registry = std::make_shared<WorkspaceRegistry>(path);
            result.entries = result.registry->read();
        } catch (const std::exception& error) {
            result.registry.reset();
            result.error = QString::fromUtf8(error.what());
        }
        return result;
    }));
}

void Workspace::appendSession(std::unique_ptr<SessionPreview> document) {
    connect(document.get(), &SessionPreview::connectionChanged, this,
            [this] { persistRegistry(); });
    sessions_.push_back(std::move(document));
}

void Workspace::persistRegistry(bool retry) {
    if ((!registry_ready_ && !retry) || !storage_)
        return;
    if (storage_->writing) {
        storage_->dirty = true;
        return;
    }
    std::vector<WorkspaceEntry> entries;
    for (const auto& document : sessions_)
        if (const auto entry = document->reconnectEntry())
            entries.push_back(*entry);
    if (!retry && entries == storage_->written)
        return;
    storage_->writing = true;
    storage_->dirty = false;
    disconnect(&storage_->save, nullptr, this, nullptr);
    connect(&storage_->save, &QFutureWatcher<QString>::finished, this, [this, entries] {
        storage_->writing = false;
        const auto error = storage_->save.result();
        if (!error.isEmpty()) {
            registry_ready_ = false;
            status_ = QStringLiteral("Workspace could not be saved: ") + error;
        } else {
            registry_ready_ = true;
            storage_->written = entries;
            status_ = QString{};
            if (storage_->dirty)
                persistRegistry();
        }
        emit workspaceChanged();
    });
    if (retry)
        status_ = QStringLiteral("Retrying workspace save…");
    storage_->save.setFuture(background<QString>([registry = storage_->registry, entries] {
        try {
            registry->write(entries);
            return QString{};
        } catch (const std::exception& error) {
            return QString::fromUtf8(error.what());
        }
    }));
    emit workspaceChanged();
}

bool Workspace::addSession(bool codex, const QString& directory, const QString& endpoint) {
    return createSession(codex ? session::AgentMode::codex : session::AgentMode::terminal,
                         directory, endpoint);
}

bool Workspace::addClaudeSession(const QString& directory, const QString& endpoint) {
    return createSession(session::AgentMode::claude, directory, endpoint);
}

bool Workspace::createSession(session::AgentMode agent, const QString& directory,
                              const QString& endpoint) {
    if (!canAddSessions())
        return false;
    try {
        const QString cwd = directory.isEmpty() ? rootDirectory() : directory;
        auto launch = session::shell_launch(cwd);
        if (agent != session::AgentMode::terminal) {
            launch.program = QStandardPaths::findExecutable(agent == session::AgentMode::codex
                                                                ? QStringLiteral("codex")
                                                                : QStringLiteral("claude"));
            launch.arguments.clear();
            launch.agent = agent;
        }
        launch = session::validate_launch(std::move(launch));
        const QString path = session::posix::prepare_endpoint(
            endpoint.isEmpty() ? QFileInfo{manifest_}.absoluteDir().filePath(
                                     QStringLiteral("session-") +
                                     QString::fromLatin1(session::wire::new_id().toHex()) +
                                     QStringLiteral(".sock"))
                               : endpoint);
        for (const auto& document : sessions_)
            if (document->property("workspaceEndpoint").toString() == path ||
                (document->reconnectEntry() && document->reconnectEntry()->endpoint == path))
                throw std::invalid_argument("That endpoint is already in this workspace");
        auto document = std::make_unique<SessionPreview>(
            agent == session::AgentMode::terminal ? QStringLiteral("Shell")
            : agent == session::AgentMode::codex  ? QStringLiteral("Codex")
                                                  : QStringLiteral("Claude"),
            launch.directory, QStringLiteral("Connecting"), QColor{"#87cbac"}, "");
        document->setSessionId(QString::fromLatin1(session::wire::new_id().toHex()));
        document->setProperty("workspaceEndpoint", path);
        auto* added = document.get();
        appendSession(std::move(document));
        added->startLive(path, launch,
                         endpoint.isEmpty() ? session::wire::AttachMode::create
                                            : session::wire::AttachMode::discover);
        status_.clear();
        emit sessionsChanged();
        emit workspaceChanged();
        setFocusedIndex(static_cast<int>(sessions_.size() - 1));
        if (sessions_.size() == 1)
            emit focusChanged();
        return true;
    } catch (const std::exception& error) {
        status_ = QString::fromUtf8(error.what());
        emit workspaceChanged();
        return false;
    }
}

bool Workspace::removeSession(const QString& id) {
    if (!registry_ready_ || !interaction_blocks_.isEmpty())
        return false;
    const auto found = std::find_if(sessions_.begin(), sessions_.end(),
                                    [&](const auto& entry) { return entry->sessionId() == id; });
    if (found == sessions_.end())
        return false;
    const auto focused = focusedSession() ? focusedSession()->sessionId() : QString{};
    auto removed = std::move(*found);
    sessions_.erase(found);
    focused_index_ = 0;
    for (std::size_t index = 0; index < sessions_.size(); ++index)
        if (sessions_[index]->sessionId() == focused)
            focused_index_ = static_cast<int>(index);
    if (pending_focus_ == removed.get())
        pending_focus_.clear();
    emit sessionsChanged();
    emit focusChanged();
    emit workspaceChanged();
    // QML bindings see the replacement list/focus before the old object dies.
    removed.release()->deleteLater();
    persistRegistry();
    return true;
}

void Workspace::setInteractionBlocked(const QString& reason, bool blocked) {
    const auto previous = interaction_blocks_.size();
    if (blocked)
        interaction_blocks_.insert(reason);
    else
        interaction_blocks_.remove(reason);
    if (previous != interaction_blocks_.size())
        emit interactionChanged();
    if (interaction_blocks_.isEmpty() && !pending_focus_.isNull())
        QTimer::singleShot(0, this, [this] { flushFocus(); });
}

void Workspace::flushFocus() {
    if (!interaction_blocks_.isEmpty())
        return;
    const auto target = std::exchange(pending_focus_, QPointer<SessionPreview>{});
    for (std::size_t index = 0; index < sessions_.size(); ++index)
        if (sessions_[index].get() == target) {
            setFocusedIndex(static_cast<int>(index));
            return;
        }
}

SessionPreview::SessionPreview(QString title, QString directory, QString activity, QColor accent,
                               std::string_view content)
    : title_(std::move(title)), directory_(std::move(directory)), activity_(std::move(activity)),
      accent_(accent) {
    session::Terminal terminal({100, 30});
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
    : manifest_(std::move(options.manifest)),
      default_session_directory_(QStandardPaths::writableLocation(QStandardPaths::HomeLocation)),
      preview_mode_(mode == WorkspaceMode::preview) {
    if (!manifest_.isEmpty()) {
        if (preview_mode_ || options.launch || !options.endpoint.isEmpty())
            throw std::invalid_argument(
                "Workspace manifest cannot be combined with an explicit session or preview");
        loading_ = true;
        status_ = QStringLiteral("Opening workspace…");
        loadRegistry();
        return;
    }
    const auto add = [this](const char* title, const char* directory, const char* activity,
                            const char* accent, std::string_view content) {
        sessions_.push_back(std::make_unique<SessionPreview>(
            QString::fromUtf8(title), QString::fromUtf8(directory), QString::fromUtf8(activity),
            QColor(QString::fromLatin1(accent)), content));
    };
    if (preview_mode_ && (options.launch || !options.endpoint.isEmpty()))
        throw std::invalid_argument("UI preview cannot launch or attach to a process");
    std::optional<session::LaunchSpec> launch;
    if (!preview_mode_)
        launch = options.launch ? session::validate_launch(std::move(*options.launch))
                                : session::shell_launch(rootDirectory());
    const QString directory = launch ? launch->directory : rootDirectory();
    add("Shell", directory.toUtf8().constData(), "Connecting", "#87cbac", "");
    if (!preview_mode_) {
        const QString endpoint = session::posix::prepare_endpoint(
            options.endpoint.isEmpty() ? defaultEndpoint() : options.endpoint);
        sessions_.front()->startLive(endpoint, *launch, options.mode);
        sessions_.front()->setSessionId(QStringLiteral("shell"));
        return;
    } else {
        session::Terminal terminal({100, 30});
        terminal.feed(kPreviewPalette);
        terminal.feed("~/lapis\r\n\r\n> Ready for the next step.\r\n\r\n"
                      "  The focused terminal stays readable.\r\n"
                      "  Neighboring sessions surface requests below.\r\n"
                      "  Attention never takes keyboard ownership.\r\n");
        sessions_.front()->applySnapshot(terminal.snapshot());
        sessions_.front()->setActivity(QStringLiteral("UI preview"));
    }
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
}

QVariantList Workspace::sessions() const {
    QVariantList result;
    result.reserve(static_cast<qsizetype>(sessions_.size()));
    for (const auto& session : sessions_)
        result.push_back(QVariant::fromValue(session.get()));
    return result;
}

SessionPreview* Workspace::focusedSession() const {
    return sessions_.empty() ? nullptr
                             : sessions_.at(static_cast<std::size_t>(focused_index_)).get();
}

void Workspace::setFocusedIndex(int index) {
    if (index < 0 || static_cast<std::size_t>(index) >= sessions_.size())
        return;
    emit manualNavigationRequested();
    if (index == focused_index_) {
        pending_focus_.clear();
        return;
    }
    if (!interaction_blocks_.isEmpty()) {
        pending_focus_ = sessions_[static_cast<std::size_t>(index)].get();
        return;
    }
    pending_focus_.clear();
    focused_index_ = index;
    emit focusChanged();
}

bool Workspace::focusAutomatically(const QString& id) {
    if (interactionBlocked() || pending_focus_ || !focusedSession() ||
        !focusedSession()->inputReady())
        return false;
    for (std::size_t index = 0; index < sessions_.size(); ++index) {
        const auto& target = sessions_[index];
        if (target->sessionId() != id)
            continue;
        if (target.get() == focusedSession() || !target->inputReady())
            return false;
        focused_index_ = static_cast<int>(index);
        emit focusChanged();
        return true;
    }
    return false;
}

void Workspace::nextSession(int delta) {
    if (sessions_.empty() || delta == 0)
        return;
    const int count = static_cast<int>(sessions_.size());
    // Wrap so held repeats cycle rather than sticking at an end.
    const int next = ((focused_index_ + delta % count) % count + count) % count;
    setFocusedIndex(next);
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
} // namespace lapis::desktop
