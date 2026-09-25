#include "agent_search.hpp"
#include "alerts.hpp"
#include "app_paths.hpp"
#include "desktop_actions.hpp"
#include "keymap.hpp"
#include "platform_desktop.hpp"
#include "platform_preferences.hpp"
#include "shell_environment.hpp"
#include "terminal_surface.hpp"
#include "ui_capture.hpp"
#include "ui_preview.hpp"
#include "usage.hpp"
#include "workspace_control.hpp"

#include "launch_spec.hpp"

#include <QCommandLineParser>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QPointer>
#include <QQmlEngine>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QStandardPaths>
#include <QThread>
#include <algorithm>
#include <exception>
#include <optional>
#include <set>

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
// agent. A notification for the same moments while lapis is in the
// background, clicking one brings the window to that agent. The downloaded app
// also starts checking for updates here.
void alert_for_agents(std::optional<lapis::desktop::Alerts>& alerts,
                      std::optional<lapis::desktop::Notifier>& notifier,
                      lapis::desktop::Workspace& workspace, lapis::desktop::KeyMap& keymap,
                      QPointer<QQuickWindow>& shown) {
    namespace platform = lapis::desktop::platform;
    using lapis::desktop::Chime;
    alerts.emplace(
        workspace, keymap,
        [](Chime chime) { lapis::desktop::play_sound(lapis::desktop::chime_wav(chime)); },
        [&workspace, &shown](const lapis::desktop::SessionPreview* item) {
            return shown && shown->isActive() && workspace.focusedSession() == item;
        });
    notifier.emplace(
        workspace, keymap,
        [](const QString& id, const QString& title, const QString& body) {
            platform::post_notification(id, title, body);
        },
        [] { return QGuiApplication::applicationState() != Qt::ApplicationActive; });
    platform::on_notification_opened([&workspace, &shown](const QString& id) {
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
lapis::desktop::TokenLedger::Roots transcript_roots() {
    const auto home = [](const char* variable, const char* fallback) {
        const auto set = qEnvironmentVariable(variable);
        return set.isEmpty() ? QDir::home().filePath(QLatin1String(fallback)) : set;
    };
    return {home("CODEX_HOME", ".codex") + QStringLiteral("/sessions"),
            home("CLAUDE_CONFIG_DIR", ".claude") + QStringLiteral("/projects")};
}
// A CLI usage asks, or ssh, as found on this Mac.
QString usage_program(const QString& id) {
    return id == QLatin1String("ssh") ? QStandardPaths::findExecutable(id)
                                      : lapis::desktop::harness_program(id);
}
// Usage asks the CLIs only while its setting is on, on the machines and in
// the order the config names.
void follow_usage_setting(lapis::desktop::Usage& usage, const lapis::desktop::KeyMap& keymap) {
    const auto show = [&usage, &keymap] {
        usage.setMachines(keymap.usageMachines());
        usage.setMeterOrder(keymap.usageMeter());
        usage.setActive(keymap.showUsage());
    };
    show();
    QObject::connect(&keymap, &lapis::desktop::KeyMap::changed, &usage, show);
}

int run_headless(lapis::desktop::Workspace& workspace, bool serve) {
    using lapis::desktop::SessionPreview;
    using lapis::desktop::WorkspaceControl;
    if (!workspace.workspaceError().isEmpty()) {
        // An open window holds the workspace, restores agents and serves the phone.
        qInfo().noquote() << "lapis restore:" << workspace.workspaceError();
        return 0;
    }
    std::optional<WorkspaceControl> control;
    bool handed_over = false;
    if (serve) {
        control.emplace(workspace, true);
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
        // shows from another app; a new request bounces the icon once.
#ifdef Q_OS_MACOS
        // Linux badges need an installed desktop file; the Dock needs nothing.
        const auto badge = [&workspace] { qGuiApp->setBadgeNumber(workspace.attentionAgents()); };
        QObject::connect(&workspace, &lapis::desktop::Workspace::categoriesChanged, &window, badge);
        badge();
        QObject::connect(qApp, &QCoreApplication::aboutToQuit, &window,
                         [] { qGuiApp->setBadgeNumber(0); });
#endif
        QObject::connect(&workspace, &lapis::desktop::Workspace::requestArrived, &window,
                         [&window] {
                             if (!window.isActive())
                                 window.alert(1000);
                         });
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
    parser.process(arguments);
    if (!valid_options(parser) || !valid_connection_options(parser))
        return 2;
    // Before any agent, session service or CLI probe inherits the environment.
    lapis::desktop::adopt_login_environment();
    if (qEnvironmentVariableIsEmpty("QT_VULKAN_LIB"))
        qputenv("QT_VULKAN_LIB", QFile::encodeName(lapis::desktop::vulkan_library()));
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Vulkan);
    try {
        using namespace lapis::desktop;
        const bool isolated = parser.isSet(QStringLiteral("ui-preview"));
        const auto options = workspace_options(parser, isolated);
        Workspace workspace(isolated ? WorkspaceMode::preview : WorkspaceMode::live, options);
        // The config file applies live: a change from the window or an agent
        // reaches new agents at once, with or without a window.
        KeyMap keymap;
        keymap.load();
        const auto configure = [&] {
            workspace.setHarnessArguments(keymap.harnessArguments());
            workspace.setAgentDefaults(keymap.agentDefaults());
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
            return run_headless(workspace, serve);
        // The window owns the workspace: the phone's requests come here.
        std::optional<WorkspaceControl> control;
        if (options.restoreAgents && workspace.workspaceError().isEmpty())
            control.emplace(workspace, false);

        qInfo().noquote() << "lapis keymap:" << keymap.sourcePath()
                          << (keymap.loaded() ? "loaded" : "defaults");
        qmlRegisterUncreatableType<SessionPreview>("Lapis", 1, 0, "SessionPreview",
                                                   "Sessions are owned by the workspace");
        qmlRegisterType<TerminalSurface>("Lapis", 1, 0, "TerminalSurface");
        qmlRegisterUncreatableType<KeyMap>("Lapis", 1, 0, "KeyMap",
                                           "The keymap is owned by the application");
        const auto source =
            parser.isSet(QStringLiteral("qml"))
                ? QUrl::fromLocalFile(
                      QFileInfo(parser.value(QStringLiteral("qml"))).absoluteFilePath())
                : QUrl(QStringLiteral("qrc:/qml/Main.qml"));
        QPointer<QQuickWindow> shown;
        std::optional<Alerts> alerts;
        std::optional<Notifier> notifier;
        if (!isolated)
            alert_for_agents(alerts, notifier, workspace, keymap, shown);
        DesktopActions desktop(keymap);
        AgentSearch agentSearch(&workspace);
        // Plan limits and token totals, only while the setting is on and only
        // in the real workspace. Each CLI keeps its transcripts where its own
        // home variable says.
        std::optional<Usage> usage;
        if (!isolated)
            follow_usage_setting(usage.emplace(&usage_program, transcript_roots()), keymap);
        UiPreview view(workspace, {.source = source,
                                   .compact = parser.isSet(QStringLiteral("compact")),
                                   .screen = parser.isSet(QStringLiteral("screen"))
                                                 ? parser.value(QStringLiteral("screen"))
                                                 : qEnvironmentVariable("LAPIS_SCREEN"),
                                   .keymap = &keymap,
                                   .alerts = alerts ? &*alerts : nullptr,
                                   .agentSearch = &agentSearch,
                                   .usage = usage ? &*usage : nullptr,
                                   .desktop = &desktop,
                                   .persistGeometry = !isolated && !options.launch &&
                                                      options.endpoint.isEmpty() &&
                                                      !parser.isSet(QStringLiteral("capture"))});
        view.setSystemReducedMotion(system_reduced_motion());
        view.setReducedMotion(parser.isSet(QStringLiteral("reduced-motion")));
        QObject::connect(&app, &QGuiApplication::applicationStateChanged, &view,
                         [&view](Qt::ApplicationState state) {
                             if (state == Qt::ApplicationActive)
                                 view.setSystemReducedMotion(system_reduced_motion());
                         });
        QObject::connect(&view, &UiPreview::windowChanged, &view, [&](QQuickWindow* window) {
            shown = window;
            wire_window(*window, view, workspace, parser);
            const auto update_chrome = [window] { style_window_chrome(*window); };
            QObject::connect(window, &QQuickWindow::colorChanged, window, update_chrome);
            QObject::connect(window, &QWindow::visibilityChanged, window, update_chrome);
            update_chrome();
        });
        if (!view.load())
            return 1;
        shown = view.window();
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
