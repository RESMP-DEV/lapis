#include "keymap.hpp"
#include "platform_preferences.hpp"
#include "terminal_surface.hpp"
#include "ui_capture.hpp"
#include "ui_preview.hpp"

#include "launch_spec.hpp"

#include <QCommandLineParser>
#include <QDebug>
#include <QFileInfo>
#include <QGuiApplication>
#include <QQmlEngine>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <exception>

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
    parser.addOption({QStringLiteral("new-session"),
                      QStringLiteral("Explicitly start a new session on an unused endpoint")});
    parser.addOption({QStringLiteral("discover"),
                      QStringLiteral("Explicitly discover and remember an existing session")});
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

lapis::desktop::WorkspaceOptions workspace_options(const QCommandLineParser& parser,
                                                   bool isolated) {
    lapis::desktop::WorkspaceOptions options;
    if (!isolated) {
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
                                 : QString::fromUtf8(LAPIS_PROJECT_ROOT),
                .agent = parser.isSet(QStringLiteral("codex")) ? lapis::session::AgentMode::codex
                         : parser.isSet(QStringLiteral("claude"))
                             ? lapis::session::AgentMode::claude
                             : lapis::session::AgentMode::terminal,
            };
        } else if (parser.isSet(QStringLiteral("development-shell")) ||
                   parser.isSet(QStringLiteral("smoke-input"))) {
            options.launch = lapis::session::shell_launch(
                parser.isSet(QStringLiteral("cwd")) ? parser.value(QStringLiteral("cwd"))
                                                    : QString::fromUtf8(LAPIS_PROJECT_ROOT));
        }
        // The normal workspace restarts agents whose services are gone.
        options.restoreAgents = !options.launch && options.endpoint.isEmpty();
        options.updateHarnesses = options.restoreAgents;
    }
    return options;
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
    QGuiApplication app(application_argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("lapis"));
    QCoreApplication::setOrganizationName(QStringLiteral("lapis"));
    QCommandLineParser parser;
    parser.setOptionsAfterPositionalArgumentsMode(QCommandLineParser::ParseAsPositionalArguments);
    add_options(parser);
    parser.process(arguments);
    if (!valid_options(parser) || !valid_connection_options(parser))
        return 2;
    if (qEnvironmentVariableIsEmpty("QT_VULKAN_LIB"))
        qputenv("QT_VULKAN_LIB", LAPIS_VULKAN_LIBRARY);
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Vulkan);
    try {
        using namespace lapis::desktop;
        const bool isolated = parser.isSet(QStringLiteral("ui-preview"));
        const auto options = workspace_options(parser, isolated);
        Workspace workspace(isolated ? WorkspaceMode::preview : WorkspaceMode::live, options);
        KeyMap keymap;
        keymap.load();
        workspace.setHarnessArguments(keymap.harnessArguments());
        QObject::connect(&keymap, &KeyMap::changed, &workspace,
                         [&] { workspace.setHarnessArguments(keymap.harnessArguments()); });
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
        UiPreview view(workspace, {.source = source,
                                   .compact = parser.isSet(QStringLiteral("compact")),
                                   .screen = parser.isSet(QStringLiteral("screen"))
                                                 ? parser.value(QStringLiteral("screen"))
                                                 : qEnvironmentVariable("LAPIS_SCREEN"),
                                   .keymap = &keymap,
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
            wire_window(*window, view, workspace, parser);
            const auto update_chrome = [window] { style_window_chrome(*window); };
            QObject::connect(window, &QQuickWindow::colorChanged, window, update_chrome);
            QObject::connect(window, &QWindow::visibilityChanged, window, update_chrome);
            update_chrome();
        });
        if (!view.load())
            return 1;
        view.window()->requestActivate();
        qInfo() << "UI preview:" << isolated
                << "system reduced motion:" << view.systemReducedMotion();
        return app.exec();
    } catch (const std::exception& error) {
        qCritical().noquote() << error.what();
        return 1;
    }
}
