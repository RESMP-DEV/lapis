#include "agent_search.hpp"
#include "agent_state.hpp"
#include "alerts.hpp"
#include "app_paths.hpp"
#include "conversation_index.hpp"
#include "desktop_actions.hpp"
#include "gui_state.hpp"
#include "interaction_recorder.hpp"
#include "keymap.hpp"
#include "limit_resets.hpp"
#include "next_prompt.hpp"
#include "open_request.hpp"
#include "plan_sign_in.hpp"
#include "platform_desktop.hpp"
#include "platform_preferences.hpp"
#include "shell_environment.hpp"
#include "terminal_surface.hpp"
#include "terminals.hpp"
#include "tool_status.hpp"
#include "ui_capture.hpp"
#include "ui_preview.hpp"
#include "usage.hpp"
#include "workspace.hpp"
#include "workspace_control.hpp"

#include "launch_spec.hpp"

#include <QClipboard>
#include <QCommandLineParser>
#include <QDateTime>
#include <QDebug>
#include <QDesktopServices>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>
#include <QPointer>
#include <QQmlEngine>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>
#include <algorithm>
#include <cstdlib>
#include <exception>
#include <optional>
#include <set>
#include <sys/stat.h>

namespace {
void add_options(QCommandLineParser& parser) {
    parser.addHelpOption();
    parser.addOption({QStringLiteral("development-shell"),
                      QStringLiteral("Enable an explicit terminal qualification session")});
    parser.addOption(
        {QStringLiteral("codex"),
         QStringLiteral("Use managed Codex attention (requires an explicit Codex executable)")});
    parser.addOption({QStringLiteral("claude"),
                      QStringLiteral("Use Claude Code hook attention (requires an explicit "
                                     "Claude executable)")});
    parser.addOption({QStringLiteral("restore-agents"),
                      QStringLiteral("Without a window, restart agents whose services are gone "
                                     "(after a reboot), wait until they answer, and exit.")});
    parser.addOption({QStringLiteral("serve"),
                      QStringLiteral("Without a window, host the workspace for the phone (after "
                                     "--restore-agents, if given) until a lapis window opens.")});
    parser.addOption({QStringLiteral("registry"),
                      QStringLiteral("Workspace registry for --restore-agents or --serve (tests)."),
                      QStringLiteral("path")});
    parser.addOption({QStringLiteral("new-session"),
                      QStringLiteral("Explicitly start a new session on an unused endpoint")});
    parser.addOption({QStringLiteral("discover"),
                      QStringLiteral("Explicitly discover and remember an existing session")});
    parser.addOption({QStringLiteral("no-harness-updates"),
                      QStringLiteral("Do not run a supported CLI update before a new session")});
    parser.addOption({QStringLiteral("socket"),
                      QStringLiteral("Session service socket path (required for explicit launch)"),
                      QStringLiteral("path")});
    parser.addOption({QStringLiteral("cwd"),
                      QStringLiteral("Working directory for the live terminal"),
                      QStringLiteral("directory")});
    parser.addOption({QStringLiteral("ui-preview"),
                      QStringLiteral("Isolated UI fixture; never connects to a shell")});
    parser.addOption({QStringLiteral("qml"),
                      QStringLiteral("Development QML source (requires --ui-preview)"),
                      QStringLiteral("path")});
    parser.addOption(
        {QStringLiteral("compact"), QStringLiteral("Open at the minimum review size")});
    parser.addOption({QStringLiteral("screen"),
                      QStringLiteral("Open on the QScreen whose name contains this text, "
                                     "for example built-in or ultrawide "
                                     "(defaults to LAPIS_SCREEN when unset)"),
                      QStringLiteral("name")});
    parser.addOption({QStringLiteral("reduced-motion"),
                      QStringLiteral("Preview steady attention markers without motion")});
    parser.addOption({QStringLiteral("scenario"),
                      QStringLiteral("Preview scenario: none, arrival or two"),
                      QStringLiteral("name"), QStringLiteral("none")});
    parser.addOption({QStringLiteral("capture"),
                      QStringLiteral("Capture a rendered window and exit"),
                      QStringLiteral("path")});
    parser.addOption({QStringLiteral("capture-delay"),
                      QStringLiteral("Milliseconds after first rendered frame/scenario"),
                      QStringLiteral("ms"), QStringLiteral("2000")});
    parser.addOption({QStringLiteral("trace"),
                      QStringLiteral("Write bounded frame observations with --capture"),
                      QStringLiteral("path")});
    parser.addOption(
        {QStringLiteral("smoke-input"),
         QStringLiteral("Send a test command to the live shell (never allowed in UI preview)")});
    parser.addPositionalArgument(QStringLiteral("program"),
                                 QStringLiteral("Program and literal arguments after --"),
                                 QStringLiteral("[PROGRAM ARG...]"));
}
bool valid_connection_options(const QCommandLineParser& parser) {
    const bool codex = parser.isSet(QStringLiteral("codex"));
    const bool claude = parser.isSet(QStringLiteral("claude"));
    if (codex && claude) {
        qCritical("--codex and --claude are mutually exclusive");
        return false;
    }
    if ((codex || claude) && parser.positionalArguments().isEmpty()) {
        qCritical().noquote() << (codex ? "--codex requires a Codex executable after --"
                                        : "--claude requires a Claude executable after --");
        return false;
    }
    const bool create = parser.isSet(QStringLiteral("new-session"));
    const bool discover = parser.isSet(QStringLiteral("discover"));
    if (create && discover) {
        qCritical("--new-session and --discover are mutually exclusive");
        return false;
    }
    if ((create || discover) && !parser.isSet(QStringLiteral("socket")) &&
        parser.positionalArguments().isEmpty()) {
        qCritical("--new-session and --discover require --socket or an explicit program");
        return false;
    }
    if ((create || discover) && parser.isSet(QStringLiteral("ui-preview"))) {
        qCritical("Session actions cannot be combined with --ui-preview");
        return false;
    }
    return true;
}
bool valid_agent_launch(const QCommandLineParser& parser, bool preview) {
    if (!preview && !parser.isSet(QStringLiteral("codex")) &&
        !parser.isSet(QStringLiteral("claude")) &&
        !parser.isSet(QStringLiteral("development-shell")) &&
        !parser.isSet(QStringLiteral("smoke-input")) &&
        (!parser.positionalArguments().isEmpty() || parser.isSet(QStringLiteral("socket")) ||
         parser.isSet(QStringLiteral("cwd")))) {
        qCritical("Unqualified workspace launches are rejected; use --codex, --claude, "
                  "--development-shell or --smoke-input to qualify the launch");
        return false;
    }
    return true;
}
// Managed agents need a live service; the isolated preview has none.
bool valid_preview_agent(const QCommandLineParser& parser, bool preview) {
    if (!preview)
        return true;
    for (const auto* option : {"codex", "claude"})
        if (parser.isSet(QString::fromLatin1(option))) {
            qCritical("--%s cannot be combined with --ui-preview", option);
            return false;
        }
    return true;
}
bool valid_options(const QCommandLineParser& parser) {
    const bool preview = parser.isSet(QStringLiteral("ui-preview"));
    if (!valid_preview_agent(parser, preview) || !valid_agent_launch(parser, preview))
        return false;
    const bool explicit_launch =
        !parser.positionalArguments().isEmpty() || parser.isSet(QStringLiteral("cwd"));
    if (parser.isSet(QStringLiteral("socket")) &&
        parser.value(QStringLiteral("socket")).isEmpty()) {
        qCritical("--socket requires a path");
        return false;
    }
    if (preview && (parser.isSet(QStringLiteral("socket")) || explicit_launch)) {
        qCritical("--ui-preview cannot be combined with --socket, --cwd, or an explicit program");
        return false;
    }
    if (parser.isSet(QStringLiteral("smoke-input")) && explicit_launch) {
        qCritical("--smoke-input cannot target an explicit launch or working directory");
        return false;
    }
    if (explicit_launch && !parser.isSet(QStringLiteral("socket"))) {
        qCritical("An explicit launch or working directory requires --socket");
        return false;
    }
    const auto scenario = parser.value(QStringLiteral("scenario"));
    bool delay_valid = false;
    const int delay = parser.value(QStringLiteral("capture-delay")).toInt(&delay_valid);
    if (preview && parser.isSet(QStringLiteral("smoke-input"))) {
        qCritical("--ui-preview cannot be combined with --smoke-input");
        return false;
    }
    if (!preview && (parser.isSet(QStringLiteral("qml")) || scenario != QStringLiteral("none"))) {
        qCritical("--qml and attention scenarios require --ui-preview");
        return false;
    }
    if (scenario != QStringLiteral("none") && scenario != QStringLiteral("arrival") &&
        scenario != QStringLiteral("two")) {
        qCritical("Unknown preview scenario");
        return false;
    }
    if (!delay_valid || delay < 0 || delay > 10000) {
        qCritical("--capture-delay must be between 0 and 10000 milliseconds");
        return false;
    }
    if (!parser.isSet(QStringLiteral("capture")) &&
        (parser.isSet(QStringLiteral("trace")) || parser.isSet(QStringLiteral("smoke-input")) ||
         scenario != QStringLiteral("none"))) {
        qCritical("--trace, --smoke-input and --scenario require --capture");
        return false;
    }
    return true;
}

// --restore-agents and --serve run without a window: the login helper restarts
// agents, and the host serves the phone until a window takes the workspace.
void headless_options(const QCommandLineParser& parser, lapis::desktop::WorkspaceOptions& options) {
    const bool restore = parser.isSet(QStringLiteral("restore-agents"));
    if (!restore && !parser.isSet(QStringLiteral("serve")))
        return;
    options.restoreAgents = restore;
    options.headless = true;
    options.updateHarnesses = false;
    if (parser.isSet(QStringLiteral("registry")))
        options.storagePath = parser.value(QStringLiteral("registry"));
}

lapis::desktop::WorkspaceOptions workspace_options(const QCommandLineParser& parser,
                                                   bool isolated) {
    lapis::desktop::WorkspaceOptions options;
    if (isolated) {
        headless_options(parser, options);
        return options;
    }
    if (parser.isSet(QStringLiteral("new-session")))
        options.mode = lapis::session::wire::AttachMode::create;
    else if (parser.isSet(QStringLiteral("discover")))
        options.mode = lapis::session::wire::AttachMode::discover;
    if (parser.isSet(QStringLiteral("socket")))
        options.endpoint = QFileInfo(parser.value(QStringLiteral("socket"))).absoluteFilePath();
    const auto positional = parser.positionalArguments();
    if (!positional.isEmpty()) {
        options.launch = lapis::session::LaunchSpec{
            .program = positional.front(),
            .arguments = positional.mid(1),
            .directory = parser.isSet(QStringLiteral("cwd"))
                             ? parser.value(QStringLiteral("cwd"))
                             : lapis::desktop::default_working_directory(),
            .agent = parser.isSet(QStringLiteral("codex"))    ? lapis::session::AgentMode::codex
                     : parser.isSet(QStringLiteral("claude")) ? lapis::session::AgentMode::claude
                                                              : lapis::session::AgentMode::terminal,
        };
    } else if (parser.isSet(QStringLiteral("development-shell")) ||
               parser.isSet(QStringLiteral("smoke-input"))) {
        options.launch = lapis::session::shell_launch(
            parser.isSet(QStringLiteral("cwd")) ? parser.value(QStringLiteral("cwd"))
                                                : lapis::desktop::default_working_directory());
    }
    // The normal workspace restarts agents whose services are gone.
    options.restoreAgents = !options.launch && options.endpoint.isEmpty();
    // Updates are for creating an agent. Existing-session discovery and
    // reconnection never change the CLI that owns the attached session.
    // The explicit opt-out is absolute for reproducible qualification.
    options.updateHarnesses =
        !parser.isSet(QStringLiteral("no-harness-updates")) &&
        (options.restoreAgents || parser.isSet(QStringLiteral("new-session")));
    headless_options(parser, options);
    return options;
}

bool parsed_headless(const QCommandLineParser& parser, bool parsed) {
    return parsed && (parser.isSet(QStringLiteral("serve")) ||
                      parser.isSet(QStringLiteral("restore-agents")));
}

// Chimes for agents that need you, in the real workspace (the preview
// fixtures stay silent); looking means the window is active and showing that
// agent to someone at the Mac. A notification for the same moments while
// lapis is in the background or nobody is at the Mac, clicking one brings the
// window to that agent. The downloaded app
// also starts checking for updates here.
// Every ping decision, one JSON line each, in the owner-only runtime folder;
// past 2 MiB the log starts over beside its predecessor.
// Opened from Finder or the Dock, the app's standard error is /dev/null and its
// warnings would be lost: they go to runtime/lapis.log instead (owner-only,
// timestamped, starting over beside its predecessor past 2 MiB). Run from a
// terminal or a test harness, standard error stays where it was.
bool stderr_discarded() {
    struct stat error{};
    struct stat null{};
    return ::fstat(2, &error) == 0 && ::stat("/dev/null", &null) == 0 && S_ISCHR(error.st_mode) &&
           error.st_rdev == null.st_rdev;
}
QString& app_log_path() {
    static QString path;
    return path;
}
void log_to_file(QtMsgType type, const QMessageLogContext&, const QString& message) {
    static QMutex mutex;
    const QMutexLocker lock(&mutex);
    constexpr qint64 kLimit = qint64{2} * 1024 * 1024;
    const auto& path = app_log_path();
    if (path.isEmpty())
        return;
    if (QFileInfo(path).size() > kLimit) {
        // Rotation keeps one predecessor. If the rename fails, leave the oversized
        // current log untouched and retry on a later message rather than losing it.
        const auto backup = path + QStringLiteral(".1");
        QFile::remove(backup);
        if (!QFile::rename(path, backup)) {
            // Qt logging cannot be re-entered from this handler, and this code is
            // installed only when stderr is /dev/null; record the failure in place.
            QFile current(path);
            if (current.open(QIODevice::Append | QIODevice::WriteOnly,
                             QFile::ReadOwner | QFile::WriteOwner) &&
                current.setPermissions(QFile::ReadOwner | QFile::WriteOwner))
                current.write(QDateTime::currentDateTime().toString(Qt::ISODateWithMs).toUtf8() +
                              " error: Could not rotate app log: " + backup.toUtf8() + '\n');
            return;
        }
    }
    QFile file(path);
    if (!file.open(QIODevice::Append | QIODevice::WriteOnly, QFile::ReadOwner | QFile::WriteOwner))
        return;
    if (!file.setPermissions(QFile::ReadOwner | QFile::WriteOwner))
        return;
    // QtMsgType appended QtInfoMsg last for ABI stability.
    static const char* const levels[] = {"debug", "warning", "critical", "fatal", "info"};
    const auto level = static_cast<std::size_t>(type) < std::size(levels) ? levels[type] : "log";
    const auto bytes = QDateTime::currentDateTime().toString(Qt::ISODateWithMs).toUtf8() + ' ' +
                       level + ": " + message.toUtf8() + '\n';
    const auto written = file.write(bytes);
    if (type == QtFatalMsg) {
        file.flush();
        // Installing a handler takes over Qt's fatal behavior; keep it.
        std::abort();
    }
    if (written != bytes.size())
        return;
}
void keep_app_log() {
    if (!stderr_discarded())
        return;
    const QDir runtime(QDir(lapis::desktop::data_directory()).filePath(QStringLiteral("runtime")));
    if (!runtime.exists())
        return;
    app_log_path() = runtime.filePath(QStringLiteral("lapis.log"));
    qInstallMessageHandler(log_to_file);
}
lapis::desktop::AttentionLog attention_log() {
    return lapis::desktop::attention_log(
        QDir(lapis::desktop::data_directory()).filePath(QStringLiteral("runtime/attention.jsonl")));
}
void alert_for_agents(std::optional<lapis::desktop::Alerts>& alerts,
                      std::optional<lapis::desktop::Notifier>& notifier,
                      std::optional<lapis::desktop::SeenScreens>& seen,
                      lapis::desktop::Workspace& workspace, lapis::desktop::KeyMap& keymap,
                      QPointer<QQuickWindow>& shown) {
    namespace platform = lapis::desktop::platform;
    using lapis::desktop::Chime;
    auto sounds = std::make_shared<lapis::desktop::ChimeSounds>();
    sounds->configure(keymap);
    // Someone is at the Mac: keyboard or mouse input anywhere within
    // alerts.awayAfter seconds. Where that cannot be read, assume so.
    const auto present = [&keymap] {
        const auto idle = platform::seconds_since_input();
        return idle < 0 || idle < keymap.awayAfterSeconds();
    };
    const auto looking = [&workspace, &shown, present](const lapis::desktop::SessionPreview* item) {
        return shown && shown->isActive() && workspace.focusedSession() == item && present();
    };
    seen.emplace(workspace, looking);
    const auto log = attention_log();
    alerts.emplace(
        workspace, keymap,
        [&keymap, sounds](Chime chime) { sounds->play(chime, keymap, lapis::desktop::play_sound); },
        looking);
    alerts->setSeen(&*seen);
    alerts->setLog(log);
    QObject::connect(&keymap, &lapis::desktop::KeyMap::changed, &*alerts,
                     [sounds, &keymap] { sounds->configure(keymap); });
    notifier.emplace(
        workspace, keymap,
        [](const QString& id, const QString& title, const QString& body) {
            platform::post_notification(id, title, body);
        },
        [] { return QGuiApplication::applicationState() != Qt::ApplicationActive; });
    notifier->setSeen(&*seen);
    notifier->setLog(log);
    notifier->setPresence(present, looking);
    platform::on_notification_opened([&workspace, &shown](const QString& id) {
        lapis::desktop::interaction::cause(QStringLiteral("notification"));
        if (!workspace.selectSession(id) || !shown)
            return;
        shown->show();
        shown->raise();
        shown->requestActivate();
    });
    platform::start_updater();
}

// At login (a LaunchAgent) or by hand: restart agents whose services are gone,
// resuming their conversations, and wait until they answer. With --serve, then
// host the workspace for the phone until a lapis window asks for it. Services
// keep running either way; a window reattaches to them when it opens.
// Where Codex and Claude Code keep transcripts: their own home variables, or
// their default folders.
QString cli_home(const char* variable, const QString& fallback) {
    const auto set = qEnvironmentVariable(variable);
    return set.isEmpty() ? QDir::home().filePath(fallback) : set;
}
lapis::desktop::TokenLedger::Roots transcript_roots() {
    return {cli_home("CODEX_HOME", QStringLiteral(".codex")) + QStringLiteral("/sessions"),
            cli_home("CLAUDE_CONFIG_DIR", QStringLiteral(".claude")) + QStringLiteral("/projects")};
}
// Past Claude and Codex conversations, to resume one and to put the folders
// with the most recent work first in folder pickers; the running agents'
// folders count too.
std::unique_ptr<lapis::desktop::ConversationIndex>
conversation_index(const lapis::desktop::Workspace& workspace) {
    using lapis::desktop::SessionPreview;
    auto index = std::make_unique<lapis::desktop::ConversationIndex>(
        cli_home("CLAUDE_CONFIG_DIR", QStringLiteral(".claude")),
        cli_home("CODEX_HOME", QStringLiteral(".codex")),
        QDir(lapis::desktop::data_directory())
            .filePath(QStringLiteral("runtime/conversations.json")));
    index->setOpenFolders([&workspace] {
        QStringList folders;
        for (const auto& value : workspace.sessions())
            if (const auto* item = value.value<SessionPreview*>(); item && item->live())
                folders.append(item->directory());
        return folders;
    });
    return index;
}
// An agent that still has the name it started with takes its conversation's
// title (Claude Code's own, Codex's thread name, else the first message), so
// agents started in one folder read apart on the Mac and the phone. The index
// rescans each minute; after the first pass only changed files are read.
void follow_conversation_titles(lapis::desktop::Workspace& workspace,
                                lapis::desktop::ConversationIndex& conversations) {
    const auto apply = [&workspace, &conversations] {
        const auto current = workspace.agentConversations();
        for (auto entry = current.cbegin(); entry != current.cend(); ++entry)
            if (const auto title = conversations.titleOf(entry.value()); !title.isEmpty())
                workspace.followConversationTitle(entry.key(), title);
    };
    QObject::connect(&conversations, &lapis::desktop::ConversationIndex::changed, &workspace,
                     apply);
    auto* timer = new QTimer(&conversations);
    timer->setInterval(60'000);
    QObject::connect(timer, &QTimer::timeout, &conversations,
                     &lapis::desktop::ConversationIndex::refresh);
    timer->start();
}
// On the Mac the window closes to the Dock and lapis keeps serving agents,
// alerts and the phone until it quits.
bool closes_to_dock(bool isolated, const QCommandLineParser& parser) {
#ifdef Q_OS_MACOS
    const bool hide = !isolated && !parser.isSet(QStringLiteral("capture"));
    if (hide)
        QGuiApplication::setQuitOnLastWindowClosed(false);
    return hide;
#else
    static_cast<void>(isolated);
    static_cast<void>(parser);
    return false;
#endif
}
// Switching to lapis rereads reduced motion; a click on its Dock icon (or any
// activation) brings a closed window back.
void follow_activation(QGuiApplication& app, lapis::desktop::UiPreview& view,
                       QPointer<QQuickWindow>& shown, bool hide_on_close) {
    QObject::connect(&app, &QGuiApplication::applicationStateChanged, &view,
                     [&view, &shown, hide_on_close](Qt::ApplicationState state) {
                         if (state != Qt::ApplicationActive)
                             return;
                         view.setSystemReducedMotion(lapis::desktop::system_reduced_motion());
                         if (!hide_on_close || !shown || shown->isVisible())
                             return;
                         shown->show();
                         shown->raise();
                         shown->requestActivate();
                     });
}
// Command-` before AppKit's window cycling takes it, while that key is bound.
void route_terminal_keys(lapis::desktop::UiPreview& view, const lapis::desktop::KeyMap& keymap) {
    lapis::desktop::platform::on_terminal_keys([&view, &keymap](bool shifted) {
        auto* window = view.window();
        const auto action =
            shifted ? QStringLiteral("chooseTerminal") : QStringLiteral("toggleTerminal");
        const auto keys = keymap.sequences(action);
        const bool bound = shifted ? keys.contains(QStringLiteral("Meta+Shift+`")) ||
                                         keys.contains(QStringLiteral("Meta+~"))
                                   : keys.contains(QStringLiteral("Meta+`"));
        if (window == nullptr || !window->isActive() || !bound)
            return false;
        lapis::desktop::interaction::cause(QStringLiteral("terminal-key"));
        if (auto* recorder = lapis::desktop::InteractionRecorder::active())
            recorder->note(QStringLiteral("shortcut"), {{QStringLiteral("action"), action}});
        return QMetaObject::invokeMethod(window, shifted ? "chooseTerminal" : "toggleTerminal");
    });
}
// Command-Option-L from any app: lapis comes forward on the agent that most
// recently needed you. Only the real workspace takes the key.
void route_latest_attention(lapis::desktop::UiPreview& view, lapis::desktop::Workspace& workspace,
                            bool isolated, const QCommandLineParser& parser) {
    if (isolated || workspace.previewMode() || parser.isSet(QStringLiteral("capture")))
        return;
    const bool taken = lapis::desktop::platform::on_latest_attention_key([&view, &workspace] {
        lapis::desktop::interaction::cause(QStringLiteral("attention-key"));
        if (auto* recorder = lapis::desktop::InteractionRecorder::active())
            recorder->note(QStringLiteral("shortcut"),
                           {{QStringLiteral("action"), QStringLiteral("latestAttention")},
                            {QStringLiteral("global"), true}});
        if (auto* window = view.window()) {
            window->show();
            window->raise();
            window->requestActivate();
        }
        workspace.latestAttention();
    });
    if (!taken)
        qWarning() << "Command-Option-L is held by another app; Command-L still works in lapis";
}
// The local interaction log (off unless lapis.json turns it on), only in the
// person's own workspace: never in the isolated preview, a capture run or an
// explicit qualification launch.
std::unique_ptr<lapis::desktop::InteractionRecorder>
interaction_recorder(lapis::desktop::Workspace& workspace, const lapis::desktop::KeyMap& keymap,
                     const lapis::desktop::WorkspaceOptions& options, bool isolated,
                     const QCommandLineParser& parser) {
    using lapis::desktop::InteractionRecorder;
    // restoreAgents marks the normal workspace, without an explicit launch.
    if (isolated || !options.restoreAgents || workspace.previewMode() ||
        parser.isSet(QStringLiteral("capture")))
        return nullptr;
    auto recorder = std::make_unique<InteractionRecorder>(
        workspace,
        QDir(lapis::desktop::data_directory())
            .filePath(QStringLiteral("runtime/interaction.jsonl")),
        InteractionRecorder::Hooks{.secureInput = &lapis::desktop::platform::secure_input_enabled,
                                   .monotonicUs = {},
                                   .wallMs = {}});
    auto* raw = recorder.get();
    const auto follow = [raw, &keymap] { raw->setSettings(keymap.interactionLog()); };
    follow();
    QObject::connect(&keymap, &lapis::desktop::KeyMap::changed, raw, follow);
    QObject::connect(
        qGuiApp, &QGuiApplication::applicationStateChanged, raw, [raw](Qt::ApplicationState state) {
            const auto name = state == Qt::ApplicationActive     ? "active"
                              : state == Qt::ApplicationInactive ? "inactive"
                              : state == Qt::ApplicationHidden   ? "hidden"
                                                                 : "suspended";
            raw->note(QStringLiteral("app"), {{QStringLiteral("state"), QLatin1String(name)}});
        });
    return recorder;
}
void watch_window(lapis::desktop::InteractionRecorder* recorder, QQuickWindow* window) {
    if (recorder != nullptr)
        recorder->watchWindow(window);
}
// A monitor must never outlive this scope. Clear it in reverse construction
// order, including when load or exec unwinds after an exception.
struct TerminalKeyMonitorGuard {
    ~TerminalKeyMonitorGuard() {
        lapis::desktop::platform::on_terminal_keys({});
        lapis::desktop::platform::on_latest_attention_key({});
    }
};
void register_qml_types() {
    using namespace lapis::desktop;
    qmlRegisterUncreatableType<SessionPreview>("Lapis", 1, 0, "SessionPreview",
                                               "Sessions are owned by the workspace");
    qmlRegisterType<TerminalSurface>("Lapis", 1, 0, "TerminalSurface");
    qmlRegisterUncreatableType<KeyMap>("Lapis", 1, 0, "KeyMap",
                                       "The keymap is owned by the application");
}
QUrl qml_source(const QCommandLineParser& parser) {
    return parser.isSet(QStringLiteral("qml"))
               ? QUrl::fromLocalFile(
                     QFileInfo(parser.value(QStringLiteral("qml"))).absoluteFilePath())
               : QUrl(QStringLiteral("qrc:/qml/Main.qml"));
}
QString target_screen(const QCommandLineParser& parser) {
    return parser.isSet(QStringLiteral("screen")) ? parser.value(QStringLiteral("screen"))
                                                  : qEnvironmentVariable("LAPIS_SCREEN");
}
// A CLI usage asks, or ssh, as found on this Mac.
QString usage_program(const QString& id) {
    return id == QLatin1String("ssh") ? QStandardPaths::findExecutable(id)
                                      : lapis::desktop::harness_program(id);
}
// Usage asks the CLIs only while its setting is on, on the machines and in
// the order the config names.
// Saved Claude Code and Codex limit resets, spent as OMP does, with each
// machine's own sign-in; lapis says when it spends one or cannot.
// Only the real workspace spends resets; the object QML sees, or null.
lapis::desktop::LimitResets::AgentTarget reset_target(const lapis::desktop::Workspace& workspace,
                                                      const lapis::desktop::KeyMap& keymap,
                                                      const QString& id) {
    lapis::desktop::LimitResets::AgentTarget result;
    const auto* item = workspace.session(id);
    if (item == nullptr)
        return result;
    result.machine = workspace.agentPlace(id).value(QStringLiteral("machine")).toString();
    result.cli = item->harnessId();
    result.account = workspace.agentAccount(id);
    result.credential = workspace.agentPlanCredential(id);
    if (!result.account.isEmpty()) {
        result.refusal = QStringLiteral("the selected plan is no longer configured");
        for (const auto& account : keymap.accounts().accounts)
            if (account.cli == result.cli && account.name == result.account) {
                result.home = account.home;
                result.hasHome = account.hasHome;
                result.email = account.email;
                result.refusal.clear();
                break;
            }
    }
    return result;
}

QObject* keep_limit_resets(std::optional<lapis::desktop::LimitResets>& kept,
                           const lapis::desktop::Workspace& workspace,
                           const lapis::desktop::KeyMap& keymap, bool isolated) {
    using lapis::desktop::LimitResets;
    if (isolated)
        return nullptr;
    const auto target = [&workspace, &keymap](const QString& id) {
        return reset_target(workspace, keymap, id);
    };
    auto& resets = kept.emplace(
        [&workspace, target] {
            QVector<LimitResets::AgentTarget> result;
            for (const auto& entry : workspace.sessions())
                if (const auto* item = entry.value<lapis::desktop::SessionPreview*>())
                    result.append(target(item->sessionId()));
            return result;
        },
        target, [](const QString& id) { return QStandardPaths::findExecutable(id); },
#ifdef Q_OS_MACOS
        [] { return lapis::desktop::platform::claude_code_credentials(); },
#else
        LimitResets::Credentials{},
#endif
        QDir(lapis::desktop::data_directory()).filePath(QStringLiteral("runtime")));
    const auto follow = [&resets, &keymap] { resets.setSettings(keymap.limitResets()); };
    follow();
    QObject::connect(&keymap, &lapis::desktop::KeyMap::changed, &resets, follow);
    const auto cli = [](const QString& id) {
        return id == QLatin1String("claude") ? QStringLiteral("Claude Code")
                                             : QStringLiteral("Codex");
    };
    const auto place = [](const QString& machine) {
        return machine.isEmpty() ? QStringLiteral("this Mac") : machine;
    };
    // The lambdas take the signals' own parameters.
    // NOLINTBEGIN(bugprone-easily-swappable-parameters)
    QObject::connect(&resets, &lapis::desktop::LimitResets::spent, &resets,
                     [cli, place](const QString& machine, const QString& id, const QString& email,
                                  const QString& title, const QString& why) {
                         const QString reason = why == QLatin1String("blocked")
                                                    ? QStringLiteral(" It was at its limit.")
                                                : why == QLatin1String("expiring")
                                                    ? QStringLiteral(" It was about to expire.")
                                                    : QString();
                         lapis::desktop::platform::post_notification(
                             {}, QStringLiteral("Limit reset used"),
                             QStringLiteral("%1 on %2 (%3): %4.%5")
                                 .arg(cli(id), place(machine), email, title, reason));
                     });
    QObject::connect(
        &resets, &lapis::desktop::LimitResets::declined, &resets,
        [cli, place](const QString& machine, const QString& id, const QString& reason) {
            lapis::desktop::platform::post_notification(
                {}, QStringLiteral("No limit reset used"),
                QStringLiteral("%1 on %2: %3").arg(cli(id), place(machine), reason));
        });
    QObject::connect(
        &resets, &lapis::desktop::LimitResets::uncertain, &resets,
        [cli, place](const QString& machine, const QString& id, const QString& reason) {
            lapis::desktop::platform::post_notification(
                {}, QStringLiteral("Limit reset outcome unknown"),
                QStringLiteral("%1 on %2: %3").arg(cli(id), place(machine), reason));
        });
    // NOLINTEND(bugprone-easily-swappable-parameters)
    return &resets;
}
void follow_usage_setting(lapis::desktop::Usage& usage, const lapis::desktop::KeyMap& keymap) {
    const auto show = [&usage, &keymap] {
        // Plans lapis hands out need their home machines' limits, even with
        // the dashboard hidden.
        auto machines = keymap.usageMachines();
        for (const auto& account : keymap.accounts().accounts)
            if (account.hasHome && !account.home.isEmpty() && !machines.contains(account.home))
                machines << account.home;
        usage.setMachines(machines);
        usage.setMeterOrder(keymap.usageMeter());
        usage.setActive(keymap.showUsage() || !keymap.accounts().accounts.empty());
    };
    show();
    QObject::connect(&keymap, &lapis::desktop::KeyMap::changed, &usage, show);
}

// Predicts what the person will type next to an agent that finished a turn,
// while the setting is on and only in the real workspace (see NextPrompt).
// Command+Shift+P "Add a Claude Code plan": signs in any account and keeps
// its token for sessions on this Mac.
QObject* keep_plan_sign_in(std::optional<lapis::desktop::PlanSignIn>& kept,
                           lapis::desktop::KeyMap& keymap, bool isolated) {
    if (isolated)
        return nullptr;
    return &kept.emplace(
        lapis::desktop::PlanSignIn::Hooks{
            .program =
                [](const QString& name) {
                    return name == QLatin1String("claude") ? lapis::desktop::harness_program(name)
                                                           : QStandardPaths::findExecutable(name);
                },
            .copy = [](const QString& text) { QGuiApplication::clipboard()->setText(text); },
            .open = [](const QString& link) { QDesktopServices::openUrl(QUrl(link)); },
            .record =
                [&keymap](const QString& email, const QString& machine, const QString& expected,
                          const lapis::desktop::PlanSignIn::Prepare& prepare, QString* reason) {
                    return keymap.addPlanMachine({.cli = QStringLiteral("claude"),
                                                  .email = email,
                                                  .machine = machine,
                                                  .expectedName = expected},
                                                 reason, prepare);
                },
            .machines =
                [&keymap](const QString& email) {
                    for (const auto& account : keymap.accounts().accounts)
                        if (account.cli == QLatin1String("claude") && account.email == email)
                            return account.machines;
                    return QStringList{};
                }},
        lapis::desktop::PlanSignIn::Places{
            .helper = QDir(lapis::desktop::data_directory())
                          .filePath(QStringLiteral("runtime/plan_sign_in.py")),
            .accounts = QDir::homePath() + QStringLiteral("/.lapis/accounts")});
}
QObject* keep_next_prompt(std::optional<lapis::desktop::NextPrompt>& kept,
                          lapis::desktop::Workspace& workspace,
                          const lapis::desktop::KeyMap& keymap, bool isolated) {
    using lapis::desktop::NextPrompt;
    using lapis::desktop::SessionPreview;
    if (isolated)
        return nullptr;
    const QDir data(lapis::desktop::data_directory());
    // The log lives in the private, ignored runtime folder; one kept beside it
    // by an earlier build moves there.
    const auto log = data.filePath(QStringLiteral("runtime/next_prompt.jsonl"));
    if (const auto earlier = data.filePath(QStringLiteral("next_prompt.jsonl"));
        QFileInfo::exists(earlier) && !QFileInfo::exists(log)) {
        QDir().mkpath(data.filePath(QStringLiteral("runtime")),
                      QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        if (!QFile::rename(earlier, log))
            qWarning().noquote() << "Next prompt: could not move the earlier log into runtime/";
    }
    auto& next = kept.emplace(
        [&workspace](const QString& id) -> std::optional<NextPrompt::Agent> {
            const auto* item = workspace.session(id);
            if (item == nullptr)
                return std::nullopt;
            const auto place = workspace.agentPlace(id);
            const auto machine = place.value(QStringLiteral("machine")).toString();
            auto folder = place.value(QStringLiteral("place")).toString();
            if (!machine.isEmpty())
                folder = folder.mid(machine.size() + 1);
            return NextPrompt::Agent{machine,
                                     folder,
                                     item->harnessId(),
                                     workspace.agentConversation(id),
                                     item->title(),
                                     place.value(QStringLiteral("category")).toString(),
                                     lapis::desktop::terminal_screen_text(item->snapshot())};
        },
        [&workspace] {
            QJsonArray agents;
            for (const auto& value : workspace.sessions()) {
                const auto* item = value.value<SessionPreview*>();
                if (item == nullptr)
                    continue;
                agents.append(QJsonObject{
                    {QStringLiteral("title"), item->title()},
                    {QStringLiteral("category"), workspace.agentPlace(item->sessionId())
                                                     .value(QStringLiteral("category"))
                                                     .toString()},
                    {QStringLiteral("status"), item->statusLabel()},
                    {QStringLiteral("waiting"), item->unseen() || item->attentionPending()}});
            }
            return agents;
        },
        [](const QString& name) { return QStandardPaths::findExecutable(name); },
        NextPrompt::Files{data.filePath(QStringLiteral("runtime")), log});
    const auto follow = [&next, &keymap] { next.setSettings(keymap.nextPrompt()); };
    follow();
    // Tab learns from moves made by hand too, with the guesses then on offer.
    workspace.setGuesses([&next] { return next.readyAgents(); });
    QObject::connect(&next, &QObject::destroyed, &workspace,
                     [&workspace] { workspace.setGuesses({}); });
    QObject::connect(&keymap, &lapis::desktop::KeyMap::changed, &next, follow);
    QObject::connect(&workspace, &lapis::desktop::Workspace::turnFinished, &next,
                     [&next](SessionPreview* item) {
                         if (item != nullptr)
                             next.turnFinished(item->sessionId());
                     });
    // A turn that ended while no window watched still gets its guess.
    QObject::connect(&workspace, &lapis::desktop::Workspace::finishedWhileAway, &next,
                     [&next](SessionPreview* item) {
                         if (item != nullptr)
                             next.turnFinished(item->sessionId());
                     });
    return &next;
}

// What the window knows that a restart should keep (see GuiState), beside the
// workspace registry in runtime/: guesses, unseen marks, what was seen,
// closed agents and the window's own choices. Restored once every owner
// exists; restoring never pings. A turn that ended while no window watched
// is guessed for, without a chime.
std::unique_ptr<lapis::desktop::GuiState>
keep_gui_state(lapis::desktop::Workspace& workspace,
               std::optional<lapis::desktop::NextPrompt>& nextPrompt,
               std::optional<lapis::desktop::SeenScreens>& seenScreens, bool isolated) {
    using lapis::desktop::GuiState;
    using lapis::desktop::Workspace;
    if (isolated || workspace.storagePath().isEmpty())
        return nullptr;
    auto* const next = nextPrompt ? &*nextPrompt : nullptr;
    auto* const seen = seenScreens ? &*seenScreens : nullptr;
    auto state = std::make_unique<GuiState>(QDir(QFileInfo(workspace.storagePath()).absolutePath())
                                                .filePath(QStringLiteral("gui_state.json")));
    auto* store = state.get();
    state->load();
    workspace.restoreMarks(state->section(QStringLiteral("marks")).toObject());
    workspace.restoreClosed(state->section(QStringLiteral("closed")).toArray());
    state->addSection(QStringLiteral("marks"), [&workspace] { return workspace.saveMarks(); });
    state->addSection(QStringLiteral("closed"), [&workspace] { return workspace.saveClosed(); });
    QObject::connect(&workspace, &Workspace::marksChanged, store, &GuiState::touch);
    QObject::connect(&workspace, &Workspace::closedChanged, store, &GuiState::touch);
    if (seen != nullptr) {
        seen->restoreState(state->section(QStringLiteral("seen")).toObject());
        state->addSection(QStringLiteral("seen"), [seen] { return seen->saveState(); });
        QObject::connect(seen, &lapis::desktop::SeenScreens::changed, store, &GuiState::touch);
    }
    if (next != nullptr) {
        next->restoreState(state->section(QStringLiteral("nextPrompt")).toObject());
        state->addSection(QStringLiteral("nextPrompt"), [next] { return next->saveState(); });
        QObject::connect(next, &lapis::desktop::NextPrompt::stateChanged, store, &GuiState::touch);
    }
    QObject::connect(qApp, &QCoreApplication::aboutToQuit, store, &GuiState::flush);
    return state;
}

// What this window knows about each agent, for Ultra Tab (apps/ultratab),
// beside the registry; only from the window that owns the workspace.
void keep_agent_state(std::optional<lapis::desktop::AgentStatePublisher>& kept,
                      lapis::desktop::Workspace& workspace,
                      std::optional<lapis::desktop::NextPrompt>& next, bool isolated) {
    if (isolated || !workspace.holdsRegistry())
        return;
    kept.emplace(workspace, next ? &*next : nullptr,
                 QDir(QFileInfo(workspace.storagePath()).absolutePath())
                     .filePath(QStringLiteral("agent_state.json")));
}

// Ultra Tab's "show it in lapis" (Command-L on a card): select that agent and
// bring the window forward, as clicking its notification does.
void follow_open_requests(std::optional<lapis::desktop::OpenRequests>& kept,
                          lapis::desktop::Workspace& workspace, QPointer<QQuickWindow>& shown,
                          bool isolated) {
    if (isolated || !workspace.holdsRegistry())
        return;
    kept.emplace(QDir(QFileInfo(workspace.storagePath()).absolutePath())
                     .filePath(QStringLiteral("ultratab_open.json")),
                 [&workspace, &shown](const QString& id) {
                     lapis::desktop::interaction::cause(QStringLiteral("ultratab"));
                     // Select first: a request naming an agent this window does
                     // not hold must not steal focus from the other app.
                     if (!workspace.selectSession(id) || !shown)
                         return;
                     // Ultra Tab is a different application, so its request
                     // arrives while lapis is inactive; show/raise alone would
                     // order a window in an application macOS never foregrounds.
                     lapis::desktop::platform::activate_application();
                     shown->show();
                     shown->raise();
                     shown->requestActivate();
                 });
}

// The quick-command terminals beside this workspace, with ssh hosts from the
// user's ssh config; those still running come back.
std::unique_ptr<lapis::desktop::Terminals>
terminals_for(const lapis::desktop::Workspace& workspace) {
    if (workspace.storagePath().isEmpty())
        return nullptr;
    auto terminals = std::make_unique<lapis::desktop::Terminals>(
        QFileInfo(workspace.storagePath()).absolutePath(),
        QDir::home().filePath(QStringLiteral(".ssh/config")));
    terminals->restore();
    return terminals;
}
int run_headless(lapis::desktop::Workspace& workspace, lapis::desktop::KeyMap& keymap, bool serve) {
    using lapis::desktop::SessionPreview;
    using lapis::desktop::WorkspaceControl;
    if (!workspace.workspaceError().isEmpty()) {
        // An open window holds the workspace, restores agents and serves the phone.
        qInfo().noquote() << "lapis restore:" << workspace.workspaceError();
        return 0;
    }
    std::optional<WorkspaceControl> control;
    std::unique_ptr<lapis::desktop::Terminals> terminals;
    bool handed_over = false;
    if (serve) {
        control.emplace(workspace, true);
        control->setKeyMap(&keymap);
        QObject::connect(&*control, &WorkspaceControl::handoverRequested,
                         [&handed_over] { handed_over = true; });
    }
    std::vector<SessionPreview*> started;
    for (const auto& value : workspace.sessions())
        if (auto* item = qobject_cast<SessionPreview*>(value.value<QObject*>());
            item && item->live())
            started.push_back(item);
    // A card reads disconnected until its connection begins on the event
    // loop, so an agent has settled once it is ready, or failed after trying.
    std::set<const SessionPreview*> tried;
    const auto settled = [&tried](const SessionPreview* item) {
        const auto& state = item->connectionState();
        if (state == QStringLiteral("connecting"))
            tried.insert(item);
        return item->inputReady() || state == QStringLiteral("ended") ||
               (tried.contains(item) && state == QStringLiteral("disconnected"));
    };
    QElapsedTimer clock;
    clock.start();
    const auto all_settled = [&] {
        bool done = true;
        for (const auto* item : started)
            done = settled(item) && done; // visit every item to record attempts
        return done;
    };
    while (!handed_over && clock.elapsed() < 90000 && !all_settled()) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(20);
    }
    for (const auto* item : started)
        qInfo().noquote() << "lapis restore:" << item->title() << item->harnessId()
                          << (item->inputReady() ? QStringLiteral("running")
                                                 : item->connectionState());
    qInfo().noquote() << "lapis restore:" << started.size() << "agents restarted";
    if (serve && !handed_over) {
        // Terminals for the phone, once the agents have settled.
        terminals = terminals_for(workspace);
        control->setTerminals(terminals.get());
        qInfo().noquote() << "lapis restore: serving the workspace until a window opens";
        QEventLoop loop;
        QObject::connect(&*control, &WorkspaceControl::handoverRequested, &loop, &QEventLoop::quit);
        loop.exec();
    }
    if (handed_over)
        qInfo().noquote() << "lapis restore: handed the workspace to a window";
    return 0;
}

// Connect keyboard ownership and any requested capture to the window that
// UiPreview creates. Lives outside main() to keep main's branching flat.
void wire_window(QQuickWindow& window, lapis::desktop::UiPreview& view,
                 lapis::desktop::Workspace& workspace, const QCommandLineParser& parser) {
    QObject::connect(&window, &QQuickWindow::activeChanged, &view, [&view, &window] {
        if (window.isActive())
            view.assignTerminalFocus();
    });
    if (!workspace.previewMode()) {
        // The Dock badge counts agents waiting on you in any category, so it
        // shows from another app.
#ifdef Q_OS_MACOS
        // Linux badges need an installed desktop file; the Dock needs nothing.
        const auto badge = [&workspace] { qGuiApp->setBadgeNumber(workspace.attentionAgents()); };
        QObject::connect(&workspace, &lapis::desktop::Workspace::categoriesChanged, &window, badge);
        badge();
        QObject::connect(qApp, &QCoreApplication::aboutToQuit, &window,
                         [] { qGuiApp->setBadgeNumber(0); });
#endif
    }
    if (parser.isSet(QStringLiteral("capture")))
        capture_window(window, workspace, view,
                       {.image_path = parser.value(QStringLiteral("capture")),
                        .trace_path = parser.value(QStringLiteral("trace")),
                        .scenario = parser.value(QStringLiteral("scenario")),
                        .delay_ms = parser.value(QStringLiteral("capture-delay")).toInt(),
                        .smoke_input = parser.isSet(QStringLiteral("smoke-input"))});
}
} // namespace
int main(int argc, char** argv) {
    QStringList arguments;
    for (int index = 0; index < argc; ++index)
        arguments.append(QString::fromLocal8Bit(argv[index]));
    // Qt consumes options such as -platform even after --. Only lapis parses
    // this command line; child argv must survive QGuiApplication construction.
    int application_argc = 1;
#ifdef Q_OS_MACOS
    // Qt 6.11.2's transaction layer stalled this Vulkan surface for five seconds.
    if (!qEnvironmentVariableIsSet("QT_MTL_NO_TRANSACTION"))
        qputenv("QT_MTL_NO_TRANSACTION", "1");
#endif
    QCoreApplication::setAttribute(Qt::AA_MacDontSwapCtrlAndMeta);
    // The login helper shows nothing: no window and no Dock icon.
    // Parse the same option definitions before creating the application so a
    // child argument cannot select headless mode. process() below handles help
    // and parse errors once the application name and translation context exist.
    QCommandLineParser parser;
    parser.setOptionsAfterPositionalArgumentsMode(QCommandLineParser::ParseAsPositionalArguments);
    add_options(parser);
    const bool parsed = parser.parse(arguments);
    const bool serve = parsed && parser.isSet(QStringLiteral("serve"));
    const bool headless = parsed_headless(parser, parsed);
    if (headless && !qEnvironmentVariableIsSet("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(application_argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("lapis"));
    QCoreApplication::setOrganizationName(QStringLiteral("lapis"));
    keep_app_log();
    parser.process(arguments);
    if (!valid_options(parser) || !valid_connection_options(parser))
        return 2;
    // Before any agent, session service or CLI probe inherits the environment.
    lapis::desktop::adopt_login_environment();
    keep_app_log(); // Adoption can change LAPIS_HOME; keep the log beside it.
    // Agents draw full screen on the alternate screen, which a resize cannot
    // tear; the classic renderer redraws in place and garbles when lapis
    // resizes a terminal it drew in. Claude Code turns full screen off for
    // good after launches that end early, as lapis's closes and restarts do;
    // this variable overrides that. A value the user set is kept.
    if (!qEnvironmentVariableIsSet("CLAUDE_CODE_NO_FLICKER"))
        qputenv("CLAUDE_CODE_NO_FLICKER", "1");
    if (qEnvironmentVariableIsEmpty("QT_VULKAN_LIB"))
        qputenv("QT_VULKAN_LIB", QFile::encodeName(lapis::desktop::vulkan_library()));
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Vulkan);
    try {
        using namespace lapis::desktop;
        const bool isolated = parser.isSet(QStringLiteral("ui-preview"));
        // The config file applies live: a change from the window or an agent
        // reaches new agents at once, with or without a window. Its plans are
        // known before restored agents start.
        KeyMap keymap;
        keymap.load();
        auto options = workspace_options(parser, isolated);
        options.accounts = keymap.accounts();
        options.harnessUpdatesOff = keymap.harnessUpdatesOff();
        Workspace workspace(isolated ? WorkspaceMode::preview : WorkspaceMode::live, options);
        const auto configure = [&] {
            workspace.setHarnessArguments(keymap.harnessArguments());
            workspace.setHarnessUpdatesOff(keymap.harnessUpdatesOff());
            workspace.setAgentDefaults(keymap.agentDefaults());
            workspace.setAccounts(keymap.accounts());
            workspace.setTabAway(keymap.tabAway());
        };
        configure();
        QObject::connect(&keymap, &KeyMap::changed, &workspace, configure);
        // The models each installed CLI lists, for both new-agent forms.
        HarnessModels models(&harness_program, QDir::homePath());
        if (!isolated) {
            workspace.setHarnessModels(&models);
            models.start();
        }
        if (headless)
            return run_headless(workspace, keymap, serve);
        // The window owns the workspace: the phone's requests come here.
        std::optional<WorkspaceControl> control;
        if (options.restoreAgents && workspace.workspaceError().isEmpty()) {
            control.emplace(workspace, false);
            control->setKeyMap(&keymap);
        }
        // Plain shells beside the agents (Command-`), for this Mac and the phone.
        const auto terminals = isolated ? nullptr : terminals_for(workspace);
        if (control)
            control->setTerminals(terminals.get());

        qInfo().noquote() << "lapis keymap:" << keymap.sourcePath()
                          << (keymap.loaded() ? "loaded" : "defaults");
        register_qml_types();
        QPointer<QQuickWindow> shown;
        std::optional<lapis::desktop::SeenScreens> seen; // outlives what reads it
        std::optional<Alerts> alerts;
        std::optional<Notifier> notifier;
        if (!isolated)
            alert_for_agents(alerts, notifier, seen, workspace, keymap, shown);
        DesktopActions desktop(keymap);
        const bool hide_on_close = closes_to_dock(isolated, parser);
        AgentSearch agentSearch(&workspace);
        // Plan limits and token totals, only while the setting is on and only
        // in the real workspace. Each CLI keeps its transcripts where its own
        // home variable says.
        std::optional<Usage> usage;
        if (!isolated) {
            follow_usage_setting(usage.emplace(&usage_program, transcript_roots()), keymap);
            // Plan loads decide which plan each Claude Code and Codex session uses.
            QObject::connect(&*usage, &Usage::changed, &workspace, [&workspace, &usage] {
                workspace.setAccountLoads(usage->accountLoads());
            });
        }
        // The canonical harness catalog's read-only status answers. It exists
        // in the isolated fixture too, so the pane is testable without config.
        ToolStatus toolStatus;
        std::optional<LimitResets> limitResets;
        QObject* const resetsForQml = keep_limit_resets(limitResets, workspace, keymap, isolated);
        std::optional<NextPrompt> nextPrompt;
        QObject* const nextForQml = keep_next_prompt(nextPrompt, workspace, keymap, isolated);
        std::optional<lapis::desktop::AgentStatePublisher> agentState;
        keep_agent_state(agentState, workspace, nextPrompt, isolated);
        std::optional<lapis::desktop::OpenRequests> openRequests;
        follow_open_requests(openRequests, workspace, shown, isolated);
        std::optional<lapis::desktop::PlanSignIn> planSignIn;
        QObject* const signInForQml = keep_plan_sign_in(planSignIn, keymap, isolated);
        const auto conversations = conversation_index(workspace);
        if (!isolated) {
            follow_conversation_titles(workspace, *conversations);
            conversations->refresh();
        }
        // After every owner it saves for, so it is written before they go.
        const auto guiState = keep_gui_state(workspace, nextPrompt, seen, isolated);
        // Before the view, so the window callbacks never see it destroyed.
        const auto recorder = interaction_recorder(workspace, keymap, options, isolated, parser);
        UiPreview view(workspace, {.source = qml_source(parser),
                                   .compact = parser.isSet(QStringLiteral("compact")),
                                   .screen = target_screen(parser),
                                   .keymap = &keymap,
                                   .alerts = alerts ? &*alerts : nullptr,
                                   .agentSearch = &agentSearch,
                                   .usage = usage ? &*usage : nullptr,
                                   .tools = &toolStatus,
                                   .desktop = &desktop,
                                   .conversations = conversations.get(),
                                   .terminals = terminals.get(),
                                   .limitResets = resetsForQml,
                                   .nextPrompt = nextForQml,
                                   .planSignIn = signInForQml,
                                   .guiState = guiState.get(),
                                   .persistGeometry = !isolated && !options.launch &&
                                                      options.endpoint.isEmpty() &&
                                                      !parser.isSet(QStringLiteral("capture")),
                                   .hideOnClose = hide_on_close});
        view.setSystemReducedMotion(system_reduced_motion());
        view.setReducedMotion(parser.isSet(QStringLiteral("reduced-motion")));
        follow_activation(app, view, shown, hide_on_close);
        const TerminalKeyMonitorGuard terminal_key_monitor;
        QObject::connect(&view, &UiPreview::windowChanged, &view, [&](QQuickWindow* window) {
            shown = window;
            wire_window(*window, view, workspace, parser);
            watch_window(recorder.get(), window);
            const auto update_chrome = [window] { style_window_chrome(*window); };
            QObject::connect(window, &QQuickWindow::colorChanged, window, update_chrome);
            QObject::connect(window, &QWindow::visibilityChanged, window, update_chrome);
            update_chrome();
        });
        if (!view.load())
            return 1;
        shown = view.window();
        watch_window(recorder.get(), view.window());
        route_terminal_keys(view, keymap);
        route_latest_attention(view, workspace, isolated, parser);
        view.window()->requestActivate();
        qInfo() << "UI preview:" << isolated
                << "system reduced motion:" << view.systemReducedMotion();
        const int result = QGuiApplication::exec();
        platform::on_notification_opened({});
        return result;
    } catch (const std::exception& error) {
        qCritical().noquote() << error.what();
        return 1;
    }
}
