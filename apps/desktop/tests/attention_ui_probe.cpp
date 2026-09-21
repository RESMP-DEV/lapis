#include "platform/window_activation.hpp"
#include "terminal_surface.hpp"
#include "ui_preview.hpp"

#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QQmlEngine>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QThread>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
void until(const std::function<bool()>& condition,
           const char* message = "Desktop attention condition timed out") {
    QElapsedTimer elapsed;
    elapsed.start();
    while (!condition() && elapsed.elapsed() < 15000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
    require(condition(), message);
}
void present_layout(QQuickWindow& window) {
    const auto presented = std::make_shared<bool>(false);
    QObject::connect(
        &window, &QQuickWindow::frameSwapped, &window, [presented] { *presented = true; },
        Qt::SingleShotConnection);
    window.requestUpdate();
    until([&] { return *presented; }, "Desktop layout was not presented");
}
QQuickItem* item(QQuickItem* root, const QString& name) {
    if (root->objectName() == name)
        return root;
    for (auto* child : root->childItems())
        if (auto* found = item(child, name))
            return found;
    return nullptr;
}
void click(QQuickWindow& window, const QString& name) {
    auto* target = item(window.contentItem(), name);
    require(target && target->isVisible() && target->isEnabled(), "Control is not actionable");
    const auto position = target->mapToScene(QPointF(target->width() / 2, target->height() / 2));
    require(window.contentItem()->contains(position), "Control is outside the window");
    const auto global = window.mapToGlobal(position);
    QMouseEvent press(QEvent::MouseButtonPress, position, global, Qt::LeftButton, Qt::LeftButton,
                      Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease, position, global, Qt::LeftButton, Qt::NoButton,
                        Qt::NoModifier);
    QCoreApplication::sendEvent(&window, &press);
    QCoreApplication::sendEvent(&window, &release);
}
void key(QQuickWindow& window, Qt::Key value) {
    QKeyEvent press(QEvent::KeyPress, value, Qt::NoModifier);
    QKeyEvent release(QEvent::KeyRelease, value, Qt::NoModifier);
    QCoreApplication::sendEvent(&window, &press);
    QCoreApplication::sendEvent(&window, &release);
}
QJsonObject read_config(const QString& path) {
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "Cannot open private fixture configuration");
    const auto document = QJsonDocument::fromJson(file.readAll());
    require(document.isObject(), "Invalid fixture configuration");
    return document.object();
}
void exercise(const QJsonObject& config) {
    using namespace lapis::desktop;
    lapis::session::LaunchSpec launch;
    launch.program = config.value("program").toString();
    launch.directory = config.value("directory").toString();
    launch.agent = lapis::session::AgentMode::codex;
    for (const auto& argument : config.value("arguments").toArray())
        launch.arguments.append(argument.toString());
    Workspace workspace(WorkspaceMode::live, {.endpoint = config.value("endpoint").toString(),
                                              .launch = launch,
                                              .mode = lapis::session::wire::AttachMode::discover});
    UiPreview preview(workspace, {.source = QUrl(QStringLiteral("qrc:/qml/Main.qml")),
                                  .compact = true,
                                  .screen = QString()});
    require(preview.load(), "Cannot load production desktop QML");
    auto* window = preview.window();
    test::activate_test_window(*window);
    until([&] { return window->isActive(); });
    auto* session = workspace.focusedSession();
    until([&] { return session->attentionReady() && session->attentionCount() == 1; });
    const auto request = session->attentionRequests().first().toMap();
    require(QJsonObject::fromVariantMap(request.value("details").toMap()) ==
                config.value("details").toObject(),
            "Live request differs from the approved fixture");
    require(request.value("enabled").toBool(), "Live request is not actionable");
    auto* dialog = window->findChild<QObject*>(QStringLiteral("attentionDialog"));
    require(dialog && !dialog->property("visible").toBool(),
            "Request opened a dialog automatically");
    require(preview.assignTerminalFocus(), "Terminal did not retain focus");
    click(*window, QStringLiteral("reviewAttention"));
    until([&] { return dialog->property("opened").toBool(); });
    require(!preview.assignTerminalFocus(), "Terminal took focus from response dialog");
    click(*window, QStringLiteral("request-0"));
    until([&] { return dialog->property("canRespond").toBool(); });
    const auto answers = config.value("answers").toObject();
    const auto optionIndices = config.value("optionIndices").toObject();
    for (auto answer = answers.begin(); answer != answers.end(); ++answer) {
        const auto name = QStringLiteral("options-") + answer.key();
        const auto optionIndex = optionIndices.value(answer.key()).toInt(-1);
        require(optionIndex >= 0, "Expected option index is missing or invalid");
        present_layout(*window);
        click(*window, name);
        auto* combo = item(window->contentItem(), name);
        auto* popup = combo->property("popup").value<QObject*>();
        require(popup, "Question options popup missing");
        until([&] { return popup->property("opened").toBool(); },
              "Question options popup did not open");
        key(*window, Qt::Key_Home);
        for (auto index = 0; index < optionIndex; ++index)
            key(*window, Qt::Key_Down);
        key(*window, Qt::Key_Return);
        const auto expected = answer.value().toObject();
        until(
            [&] {
                return QJsonObject::fromVariantMap(dialog->property("answers").toMap())
                           .value(answer.key())
                           .toObject() == expected;
            },
            "Question option selection did not update the draft");
        until([&] { return !popup->property("visible").toBool(); },
              "Question options popup did not close");
    }
    until(
        [&] { return QJsonObject::fromVariantMap(dialog->property("answers").toMap()) == answers; },
        "Completed question answers do not match the fixture");
    require(window->grabWindow().save(config.value("capture").toString()), "Capture failed");
    click(*window, QStringLiteral("respond-") + config.value("choice").toString());
    require(!dialog->property("canRespond").toBool(), "Response remained enabled after submission");
    until([&] { return session->attentionReady() && session->attentionCount() == 0; });
    require(QMetaObject::invokeMethod(dialog, "close"), "Cannot close response dialog");
    until([&] { return !dialog->property("visible").toBool(); });
    std::cout << "Live request displayed, explicit Qt control response sent, source resolved\n";
}
} // namespace
int main(int argc, char** argv) {
    QCoreApplication::setAttribute(Qt::AA_MacDontSwapCtrlAndMeta);
    qputenv("QT_MTL_NO_TRANSACTION", "1");
    qputenv("QT_VULKAN_LIB", LAPIS_VULKAN_LIBRARY);
    QGuiApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Vulkan);
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    qmlRegisterUncreatableType<lapis::desktop::SessionPreview>("Lapis", 1, 0, "SessionPreview",
                                                               "Owned by workspace");
    qmlRegisterType<lapis::desktop::TerminalSurface>("Lapis", 1, 0, "TerminalSurface");
    try {
        require(app.arguments().size() == 2, "Expected a private fixture configuration path");
        exercise(read_config(app.arguments()[1]));
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
