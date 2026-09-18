#include "terminal_surface.hpp"
#include "ui_preview.hpp"
#include <QElapsedTimer>
#include <QQmlEngine>
#include <QQuickStyle>
#include <QThread>

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QEvent>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QObject>
#include <QPointer>
#include <QQuickWindow>
#include <QRect>
#include <QSGRendererInterface>
#include <QTemporaryDir>
#include <QUrl>

#include <array>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

void require(bool condition, const char* expression, int line) {
    if (!condition)
        throw std::runtime_error(std::string(expression) + " at line " + std::to_string(line));
}
#define CHECK(condition) require((condition), #condition, __LINE__)

struct AttentionRecorder final : QObject {
  public:
    int changed_count{};
    int arrived_count{};
};

QString write_qml(const QTemporaryDir& directory, const QString& name, QStringView qml) {
    const QString path = directory.filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        throw std::runtime_error("Cannot open temporary QML: " + name.toStdString());
    }
    file.write(qml.toString().toUtf8());
    file.close();
    return path;
}

int run_workspace_tests() {
    lapis::desktop::Workspace workspace(lapis::desktop::WorkspaceMode::preview);
    CHECK(workspace.previewMode());
    CHECK(workspace.focusedIndex() == 0);
    CHECK(workspace.focusedSession() != nullptr);

    const std::array<QString, 6> expected_ids = {
        QStringLiteral("shell"),   QStringLiteral("renderer"), QStringLiteral("agent"),
        QStringLiteral("service"), QStringLiteral("checks"),   QStringLiteral("notes")};
    const QVariantList sessions = workspace.sessions();
    CHECK(sessions.size() == static_cast<qsizetype>(expected_ids.size()));
    std::size_t session_index = 0;
    for (const auto& value : sessions) {
        auto* session = qvariant_cast<lapis::desktop::SessionPreview*>(value);
        CHECK(session != nullptr);
        CHECK(session->sessionId() == expected_ids.at(session_index));
        CHECK(!session->live());
        CHECK(!session->attentionPending());
        CHECK(session->attentionCount() == 0);
        CHECK(session->attentionSerial() == 0);
        ++session_index;
    }
    CHECK(workspace.session(QStringLiteral("shell"))->sessionId() == QStringLiteral("shell"));
    CHECK(workspace.session(QStringLiteral("does-not-exist")) == nullptr);

    auto* agent = workspace.session(QStringLiteral("agent"));
    AttentionRecorder recorder;
    QObject::connect(agent, &lapis::desktop::SessionPreview::attentionChanged, &recorder,
                     [&recorder] { ++recorder.changed_count; });
    QObject::connect(agent, &lapis::desktop::SessionPreview::attentionArrived, &recorder,
                     [&recorder] { ++recorder.arrived_count; });

    const quint32 initial_serial = agent->attentionSerial();
    CHECK(workspace.requestAttention(
        {QStringLiteral("agent"), QStringLiteral("build"), QStringLiteral("Review the build")}));
    CHECK(agent->attentionPending());
    CHECK(agent->attentionCount() == 1);
    CHECK(agent->attentionReason() == QStringLiteral("Review the build"));
    CHECK(agent->attentionSerial() == initial_serial + 1);
    CHECK(recorder.changed_count == 1);
    CHECK(recorder.arrived_count == 1);

    CHECK(!workspace.requestAttention(
        {QStringLiteral("agent"), QStringLiteral("build"), QStringLiteral("Duplicate request")}));
    CHECK(agent->attentionCount() == 1);
    CHECK(agent->attentionReason() == QStringLiteral("Review the build"));
    CHECK(agent->attentionSerial() == initial_serial + 1);
    CHECK(recorder.changed_count == 1);
    CHECK(recorder.arrived_count == 1);

    CHECK(workspace.requestAttention(
        {QStringLiteral("agent"), QStringLiteral("deploy"), QStringLiteral("Approve deployment")}));
    CHECK(agent->attentionCount() == 2);
    CHECK(agent->attentionSerial() == initial_serial + 2);
    CHECK(recorder.changed_count == 2);
    CHECK(recorder.arrived_count == 2);
    CHECK(workspace.resolveAttention({QStringLiteral("agent"), QStringLiteral("build"), {}}));
    CHECK(agent->attentionCount() == 1);
    CHECK(agent->attentionReason() == QStringLiteral("Approve deployment"));
    CHECK(recorder.changed_count == 3);
    CHECK(recorder.arrived_count == 2);
    CHECK(!workspace.resolveAttention({QStringLiteral("agent"), QStringLiteral("build"), {}}));

    auto* renderer = workspace.session(QStringLiteral("renderer"));
    CHECK(workspace.requestAttention({QStringLiteral("renderer"), QStringLiteral("render"),
                                      QStringLiteral("Choose presentation")}));
    CHECK(renderer->attentionPending());
    CHECK(renderer->attentionCount() == 1);
    CHECK(agent->attentionCount() == 1);
    CHECK(workspace.resolveAttention({QStringLiteral("renderer"), QStringLiteral("render"), {}}));
    CHECK(!renderer->attentionPending());
    CHECK(!workspace.resolveAttention({QStringLiteral("agent"), QStringLiteral("render"), {}}));
    CHECK(agent->attentionCount() == 1);

    CHECK(workspace.replayAttention(QStringLiteral("reset")));
    CHECK(!agent->attentionPending());
    CHECK(!renderer->attentionPending());

    CHECK(workspace.replayAttention(QStringLiteral("two")));
    CHECK(agent->attentionCount() == 1);
    CHECK(renderer->attentionCount() == 1);
    CHECK(!workspace.replayAttention(QStringLiteral("two")));

    auto* checks = workspace.session(QStringLiteral("checks"));
    for (int index = 0; index < 8; ++index)
        CHECK(workspace.requestAttention({QStringLiteral("checks"),
                                          QStringLiteral("request-%1").arg(index + 1),
                                          QStringLiteral("bounded")}));
    CHECK(checks->attentionCount() == 8);
    CHECK(checks->attentionSerial() == 8);
    CHECK(!workspace.requestAttention(
        {QStringLiteral("checks"), QStringLiteral("overflow"), QStringLiteral("bounded")}));
    CHECK(checks->attentionCount() == 8);
    CHECK(checks->attentionSerial() == 8);
    CHECK(!workspace.requestAttention(
        {QStringLiteral("checks"), QString(), QStringLiteral("empty id")}));
    CHECK(!workspace.requestAttention(
        {QStringLiteral("missing"), QStringLiteral("missing"), QStringLiteral("missing session")}));
    return EXIT_SUCCESS;
}

int run_ui_tests() {
    lapis::desktop::Workspace workspace(lapis::desktop::WorkspaceMode::preview);
    QTemporaryDir directory;
    CHECK(directory.isValid());
    const QString qml_path =
        write_qml(directory, QStringLiteral("valid.qml"),
                  QStringLiteral("import QtQuick\nWindow { id: root; objectName: \"preview-root\"; "
                                 "width: 481; height: 313; visible: false\nfunction reloadNow() { "
                                 "preview.reload(); return 7; }\n}"));

    lapis::desktop::UiPreview preview(workspace,
                                      {.source = QUrl::fromLocalFile(qml_path), .compact = true});
    CHECK(preview.active());
    CHECK(!preview.reducedMotion());
    CHECK(preview.load());
    QQuickWindow* window = preview.window();
    CHECK(window != nullptr);
    CHECK(window->objectName() == QStringLiteral("preview-root"));
    CHECK(window->width() == 980);
    CHECK(window->height() == 700);

    window->setGeometry(617, 431, 523, 337);
    const QRect preserved_geometry = window->geometry();
    const bool manual_reduced_motion = !preview.reducedMotion();
    int reduced_motion_changes = 0;
    const QMetaObject::Connection reduced_connection =
        QObject::connect(&preview, &lapis::desktop::UiPreview::reducedMotionChanged,
                         [&reduced_motion_changes] { ++reduced_motion_changes; });
    CHECK(reduced_connection);
    preview.setReducedMotion(manual_reduced_motion);
    CHECK(preview.reducedMotion());
    CHECK(reduced_motion_changes == 1);
    preview.setSystemReducedMotion(true);
    CHECK(preview.systemReducedMotion());
    CHECK(preview.reducedMotion());
    CHECK(reduced_motion_changes == 2);
    preview.setReducedMotion(false);
    CHECK(preview.reducedMotion());
    CHECK(reduced_motion_changes == 2);

    const QPointer<QQuickWindow> previous_window(window);
    write_qml(directory, QStringLiteral("valid.qml"),
              QStringLiteral("import QtQuick\nWindow { expectedInvalid: true\n}"));
    CHECK(!preview.reload());
    CHECK(previous_window == window);
    CHECK(preview.window() == window);
    CHECK(window->geometry() == preserved_geometry);
    CHECK(window->objectName() == QStringLiteral("preview-root"));
    CHECK(!preview.diagnostics().isEmpty());

    write_qml(directory, QStringLiteral("valid.qml"),
              QStringLiteral("import QtQuick\nWindow { id: root; objectName: \"preview-root\"; "
                             "width: 481; height: 313; visible: false\nfunction reloadNow() { "
                             "preview.reload(); return 7; }\n}"));
    QPointer<QQuickWindow> old_window(window);
    CHECK(QMetaObject::invokeMethod(window, "reloadNow"));
    CHECK(preview.window() != nullptr);
    CHECK(preview.window() != old_window.data());
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    CHECK(!old_window);
    CHECK(preview.window()->objectName() == QStringLiteral("preview-root"));
    CHECK(preview.window()->geometry() == preserved_geometry);
    write_qml(directory, QStringLiteral("valid.qml"),
              QStringLiteral(
                  "import QtQuick\nWindow { visible: false; "
                  "property int invalidBinding: missingValue; "
                  "function reloadTwice() { preview.reload(); preview.reload(); return 7; } }"));
    CHECK(preview.reload());
    CHECK(!preview.diagnostics().isEmpty());
    QPointer<QQuickWindow> twice = preview.window();
    CHECK(QMetaObject::invokeMethod(twice, "reloadTwice"));
    CHECK(twice); // Its QML call stack survives both reloads.
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    CHECK(!twice);
    QObject::disconnect(reduced_connection);
    return EXIT_SUCCESS;
}

QQuickItem* find_visual(QQuickItem* parent, const QString& name) {
    if (parent->objectName() == name)
        return parent;
    for (auto* child : parent->childItems())
        if (auto* match = find_visual(child, name))
            return match;
    return nullptr;
}
void pump(int milliseconds) {
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < milliseconds) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
}
int run_surface_tests() {
    using namespace lapis::desktop;
    struct KeyCase {
        int key{};
        Qt::KeyboardModifiers modifiers{};
        QString text;
        QByteArray expected;
    };
    const std::array cases{
        KeyCase{Qt::Key_B, Qt::AltModifier, QString::fromUtf8("∫"),
                QByteArray("\x1b"
                           "b")},
        KeyCase{Qt::Key_2, Qt::AltModifier, QString::fromUtf8("™"),
                QByteArray("\x1b"
                           "2")},
        KeyCase{Qt::Key_F, Qt::AltModifier, QStringLiteral("f"),
                QByteArray("\x1b"
                           "f")},
        KeyCase{Qt::Key_B,
                Qt::AltModifier | Qt::ShiftModifier,
                {},
                QByteArray("\x1b"
                           "B")},
        KeyCase{Qt::Key_Space, Qt::ControlModifier, {}, QByteArray(1, '\0')},
        KeyCase{Qt::Key_BracketLeft, Qt::ControlModifier, {}, QByteArray(1, '\x1b')},
        KeyCase{Qt::Key_Backslash, Qt::ControlModifier, {}, QByteArray(1, '\x1c')},
        KeyCase{Qt::Key_BracketRight, Qt::ControlModifier, {}, QByteArray(1, '\x1d')},
        KeyCase{Qt::Key_AsciiCircum, Qt::ControlModifier, {}, QByteArray(1, '\x1e')},
        KeyCase{Qt::Key_Underscore, Qt::ControlModifier, {}, QByteArray(1, '\x1f')},
        KeyCase{Qt::Key_C, Qt::AltModifier | Qt::ControlModifier, {}, QByteArray("\x1b\x03")},
        KeyCase{Qt::Key_E, Qt::NoModifier, QString::fromUtf8("é"), QByteArray("é")},
        KeyCase{Qt::Key_B, Qt::MetaModifier, QStringLiteral("b"), {}}};
    for (const auto& item : cases) {
        const QKeyEvent event(QEvent::KeyPress, item.key, item.modifiers, item.text);
        CHECK(terminal_text_key(event) == item.expected);
    }

    Workspace workspace(WorkspaceMode::preview);
    QQuickWindow window;
    window.resize(420, 220);
    auto* surface = new TerminalSurface(window.contentItem()); // parent owns it
    surface->setSize(window.size());
    window.show();
    pump(50); // An empty surface also crosses the render boundary.
    auto* document = workspace.focusedSession();
    surface->setDocument(document);
    pump(50);
    const QImage before = window.grabWindow();
    CHECK(!before.isNull());
    auto snapshot = document->snapshot();
    snapshot.cursor = {.column = 10, .row = 5, .in_viewport = true, .visible = true};
    document->applySnapshot(snapshot);
    const QRectF cursor = surface->inputMethodQuery(Qt::ImCursorRectangle).toRectF();
    CHECK(cursor.left() > 0 && cursor.top() > 0);
    surface->setSize(QSizeF(210, 110));
    const QRectF smaller = surface->inputMethodQuery(Qt::ImCursorRectangle).toRectF();
    CHECK(qAbs(smaller.left() * 2 - cursor.left()) < 0.01);
    CHECK(qAbs(smaller.top() * 2 - cursor.top()) < 0.01);
    for (int i = 0; i < 24; ++i) {
        snapshot.background_rgb = (i % 2 == 0) ? 0x123456U : 0x654321U;
        ++snapshot.revision;
        document->applySnapshot(snapshot);
        surface->setSize(QSizeF(420 - i, 220 - i));
        pump(2);
    }
    pump(30);
    CHECK(window.grabWindow() != before);
    snapshot.cursor.in_viewport = false;
    document->applySnapshot(snapshot);
    CHECK(surface->inputMethodQuery(Qt::ImCursorRectangle).toRectF().isEmpty());
    surface->setDocument(nullptr);
    pump(30);
    const auto empty = window.grabWindow();
    surface->setDocument(document);
    pump(30);
    CHECK(window.grabWindow() != empty);
    surface->setDocument(nullptr);
    return EXIT_SUCCESS;
}

int run_attention_ui_tests() {
    using namespace lapis::desktop;
    Workspace workspace(WorkspaceMode::preview);
    UiPreview preview(workspace, {.source = QUrl::fromLocalFile(QStringLiteral(LAPIS_QML_SOURCE))});
    CHECK(preview.load());
    auto* window = preview.window();
    window->requestActivate();
    pump(300);
    auto* terminal = find_visual(window->contentItem(), QStringLiteral("liveTerminal"));
    auto* card = find_visual(window->contentItem(), QStringLiteral("sessionCard_agent"));
    auto* renderer = find_visual(window->contentItem(), QStringLiteral("sessionCard_renderer"));
    auto* pane = find_visual(window->contentItem(), QStringLiteral("focusedPane"));
    CHECK(terminal && card && renderer && pane);
    terminal->forceActiveFocus();
    const auto pane_size = pane->size();
    const auto card_size = card->size();
    CHECK(workspace.replayAttention(QStringLiteral("arrival")));
    CHECK(card->property("cueRunning").toBool() == (window->isActive() && window->isVisible()));
    pump(1000);
    const auto serial = workspace.session(QStringLiteral("agent"))->attentionSerial();
    CHECK(!workspace.replayAttention(QStringLiteral("duplicate")));
    auto* agent = workspace.session(QStringLiteral("agent"));
    agent->applySnapshot(agent->snapshot());
    pump(500);
    CHECK(!card->property("cueRunning").toBool());
    CHECK(card->property("pending").toBool());
    CHECK(agent->attentionSerial() == serial);
    CHECK(terminal->hasFocus());
    CHECK(pane->size() == pane_size && card->size() == card_size);
    CHECK(workspace.replayAttention(QStringLiteral("reset")));
    CHECK(workspace.replayAttention(QStringLiteral("two")));
    pump(100);
    // Native focus can change while pumping events. The product intentionally
    // pauses cues in an inactive window; that is correct behavior, not a failure.
    const bool visual_active = window->isActive() && window->isVisible();
    CHECK(card->property("cueRunning").toBool() == visual_active);
    CHECK(renderer->property("cueRunning").toBool() == visual_active);
    CHECK(workspace.replayAttention(QStringLiteral("resolve")));
    CHECK(!card->property("pending").toBool());
    CHECK(renderer->property("pending").toBool());
    preview.setReducedMotion(true);
    CHECK(!renderer->property("cueRunning").toBool());
    CHECK(renderer->property("cueLevel").toDouble() == 0);
    CHECK(workspace.replayAttention(QStringLiteral("arrival")));
    CHECK(!card->property("cueRunning").toBool());
    CHECK(card->property("pending").toBool());
    CHECK(terminal->hasFocus());
    CHECK(pane->size() == pane_size && card->size() == card_size);
    CHECK(preview.diagnostics().isEmpty());
    return EXIT_SUCCESS;
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication::setAttribute(Qt::AA_MacDontSwapCtrlAndMeta);
    QGuiApplication app(argc, argv);
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Vulkan);
    QCoreApplication::setApplicationName(QStringLiteral("lapis-ui-preview-tests"));
    QCoreApplication::setOrganizationName(QStringLiteral("lapis"));
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    qmlRegisterUncreatableType<lapis::desktop::SessionPreview>("Lapis", 1, 0, "SessionPreview",
                                                               "Owned by workspace");
    qmlRegisterType<lapis::desktop::TerminalSurface>("Lapis", 1, 0, "TerminalSurface");
    try {
        if (run_workspace_tests() != EXIT_SUCCESS || run_ui_tests() != EXIT_SUCCESS ||
            run_surface_tests() != EXIT_SUCCESS || run_attention_ui_tests() != EXIT_SUCCESS)
            return EXIT_FAILURE;
        std::cout << "ui_preview_test: PASS\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "ui_preview_test: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
