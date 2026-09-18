#include "terminal_surface.hpp"
#include "workspace.hpp"

#include <QCommandLineParser>
#include <QDeadlineTimer>
#include <QDebug>
#include <QGuiApplication>
#include <QImage>
#include <QKeyEvent>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QTimer>
#include <exception>

namespace {
void send_smoke_input(QQuickWindow* window, qint64 token) {
    window->requestActivate();
    auto* terminal =
        window->findChild<lapis::desktop::TerminalSurface*>(QStringLiteral("liveTerminal"));
    if (!terminal)
        qFatal("Live terminal surface missing");
    terminal->forceActiveFocus();
    QKeyEvent discard(QEvent::KeyPress, Qt::Key_X, Qt::NoModifier, QStringLiteral("discard-this"));
    QCoreApplication::sendEvent(window, &discard);
    QKeyEvent clear_line(QEvent::KeyPress, Qt::Key_U, Qt::ControlModifier, QStringLiteral("u"));
    QCoreApplication::sendEvent(window, &clear_line);
    const QString command =
        QStringLiteral("printf '\\nLAPIS_INPUT_%s_OK\\n' %1; stty size").arg(token);
    for (const QChar character : command) {
        QKeyEvent event(QEvent::KeyPress, character.toUpper().unicode(), Qt::NoModifier,
                        QString(character));
        QCoreApplication::sendEvent(window, &event);
    }
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier, QStringLiteral("\r"));
    QCoreApplication::sendEvent(window, &enter);
}
void capture_when_ready(QQuickWindow* window, lapis::desktop::Workspace& workspace,
                        const QString& path, bool smoke) {
    // QObject parenting owns this timer for the lifetime of the review window.
    auto* timer = new QTimer(window);
    timer->setInterval(100);
    const auto token = QCoreApplication::applicationPid();
    QObject::connect(timer, &QTimer::timeout, window,
                     [timer, window, &workspace, path, smoke, token, sent = false,
                      deadline = QDeadlineTimer(15000)]() mutable {
                         if (deadline.hasExpired()) {
                             qCritical() << "Live window acceptance timed out";
                             QCoreApplication::exit(1);
                             return;
                         }
                         if (workspace.focusedSession()->activity() != QStringLiteral("Live shell"))
                             return;
                         if (smoke && !sent) {
                             send_smoke_input(window, token);
                             sent = true;
                             return;
                         }
                         if (smoke &&
                             workspace.focusedSession()->snapshot().graphemes.find(
                                 QStringLiteral("LAPIS_INPUT_%1_OK").arg(token).toStdU32String()) ==
                                 std::u32string::npos)
                             return;
                         timer->stop();
                         QTimer::singleShot(200, window, [window, path, smoke] {
                             const auto capture = window->grabWindow();
                             const bool saved = !capture.isNull() && capture.save(path);
                             qInfo() << "Window capture" << (saved ? "saved" : "failed") << path;
                             if (smoke)
                                 qInfo() << "Qt input to PTY to snapshot: PASS";
                             QCoreApplication::exit(saved ? 0 : 1);
                         });
                     });
    timer->start();
}
} // namespace

int main(int argc, char** argv) {
#ifdef Q_OS_MACOS
    // Qt 6.11.2's transaction layer stalled this Vulkan surface for five seconds.
    // The plain CAMetalLayer path was exercised with threaded Vulkan presentation.
    if (!qEnvironmentVariableIsSet("QT_MTL_NO_TRANSACTION"))
        qputenv("QT_MTL_NO_TRANSACTION", "1");
#endif
    QCoreApplication::setAttribute(Qt::AA_MacDontSwapCtrlAndMeta);
    QGuiApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("lapis"));
    QCoreApplication::setOrganizationName(QStringLiteral("lapis"));
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption({QStringLiteral("capture"),
                      QStringLiteral("Save an app-window capture and exit"),
                      QStringLiteral("path")});
    parser.addOption(
        {QStringLiteral("compact"), QStringLiteral("Open at the minimum review size")});
    parser.addOption({QStringLiteral("smoke-input"),
                      QStringLiteral("Exercise Qt key routing with a harmless shell marker")});
    parser.process(app);
    if (qEnvironmentVariableIsEmpty("QT_VULKAN_LIB"))
        qputenv("QT_VULKAN_LIB", LAPIS_VULKAN_LIBRARY);
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Vulkan);
    try {
        lapis::desktop::Workspace workspace;
        QQmlApplicationEngine engine;
        qmlRegisterUncreatableType<lapis::desktop::SessionPreview>(
            "Lapis", 1, 0, "SessionPreview", "Sessions are owned by the workspace");
        qmlRegisterType<lapis::desktop::TerminalSurface>("Lapis", 1, 0, "TerminalSurface");
        engine.rootContext()->setContextProperty(QStringLiteral("workspace"), &workspace);
        engine.load(QUrl(QStringLiteral("qrc:/qml/Main.qml")));
        if (engine.rootObjects().isEmpty())
            return 1;
        auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().front());
        if (!window)
            return 1;
        if (parser.isSet(QStringLiteral("compact")))
            window->resize(980, 700);
        QObject::connect(
            window, &QQuickWindow::sceneGraphInitialized, window,
            [window] {
                qInfo() << "lapis scene graph API:" << window->rendererInterface()->graphicsApi();
                if (window->rendererInterface()->graphicsApi() != QSGRendererInterface::Vulkan)
                    qFatal("Requested Vulkan renderer was not selected");
            },
            Qt::DirectConnection);
        window->show();
        window->requestActivate();
        if (parser.isSet(QStringLiteral("capture")))
            capture_when_ready(window, workspace, parser.value(QStringLiteral("capture")),
                               parser.isSet(QStringLiteral("smoke-input")));
        return app.exec();
    } catch (const std::exception& error) {
        qCritical().noquote() << error.what();
        return 1;
    }
}
