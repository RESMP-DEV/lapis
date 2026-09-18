#include "platform_preferences.hpp"
#include "terminal_surface.hpp"
#include "ui_capture.hpp"
#include "ui_preview.hpp"

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
    parser.addOption({QStringLiteral("ui-preview"),
                      QStringLiteral("Isolated UI fixture; never connects to a shell")});
    parser.addOption({QStringLiteral("qml"),
                      QStringLiteral("Development QML source (requires --ui-preview)"),
                      QStringLiteral("path")});
    parser.addOption(
        {QStringLiteral("compact"), QStringLiteral("Open at the minimum review size")});
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
}
bool valid_options(const QCommandLineParser& parser) {
    const bool preview = parser.isSet(QStringLiteral("ui-preview"));
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
} // namespace
int main(int argc, char** argv) {
#ifdef Q_OS_MACOS
    // Qt 6.11.2's transaction layer stalled this Vulkan surface for five seconds.
    if (!qEnvironmentVariableIsSet("QT_MTL_NO_TRANSACTION"))
        qputenv("QT_MTL_NO_TRANSACTION", "1");
#endif
    QCoreApplication::setAttribute(Qt::AA_MacDontSwapCtrlAndMeta);
    QGuiApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("lapis"));
    QCoreApplication::setOrganizationName(QStringLiteral("lapis"));
    QCommandLineParser parser;
    add_options(parser);
    parser.process(app);
    if (!valid_options(parser))
        return 2;
    if (qEnvironmentVariableIsEmpty("QT_VULKAN_LIB"))
        qputenv("QT_VULKAN_LIB", LAPIS_VULKAN_LIBRARY);
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Vulkan);
    try {
        using namespace lapis::desktop;
        const bool isolated = parser.isSet(QStringLiteral("ui-preview"));
        Workspace workspace(isolated ? WorkspaceMode::preview : WorkspaceMode::live);
        qmlRegisterUncreatableType<SessionPreview>("Lapis", 1, 0, "SessionPreview",
                                                   "Sessions are owned by the workspace");
        qmlRegisterType<TerminalSurface>("Lapis", 1, 0, "TerminalSurface");
        const auto source =
            parser.isSet(QStringLiteral("qml"))
                ? QUrl::fromLocalFile(
                      QFileInfo(parser.value(QStringLiteral("qml"))).absoluteFilePath())
                : QUrl(QStringLiteral("qrc:/qml/Main.qml"));
        UiPreview view(workspace,
                       {.source = source, .compact = parser.isSet(QStringLiteral("compact"))});
        view.setSystemReducedMotion(system_reduced_motion());
        view.setReducedMotion(parser.isSet(QStringLiteral("reduced-motion")));
        QObject::connect(&app, &QGuiApplication::applicationStateChanged, &view,
                         [&view](Qt::ApplicationState state) {
                             if (state == Qt::ApplicationActive)
                                 view.setSystemReducedMotion(system_reduced_motion());
                         });
        QObject::connect(&view, &UiPreview::windowChanged, &view, [&](QQuickWindow* window) {
            if (parser.isSet(QStringLiteral("capture")))
                capture_window(*window, workspace, view,
                               {.image_path = parser.value(QStringLiteral("capture")),
                                .trace_path = parser.value(QStringLiteral("trace")),
                                .scenario = parser.value(QStringLiteral("scenario")),
                                .delay_ms = parser.value(QStringLiteral("capture-delay")).toInt(),
                                .smoke_input = parser.isSet(QStringLiteral("smoke-input"))});
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
