#include "keymap.hpp"
#include "platform/window_activation.hpp"
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
#include <QInputMethodEvent>
#include <QJsonArray>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QObject>
#include <QPair>
#include <QPointer>
#include <QQuickWindow>
#include <QRect>
#include <QSGRendererInterface>
#include <QScreen>
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

    lapis::desktop::UiPreview preview(
        workspace, {.source = QUrl::fromLocalFile(qml_path), .compact = true, .screen = QString()});
    CHECK(preview.active());
    CHECK(!preview.reducedMotion());
    CHECK(preview.load());
    QQuickWindow* window = preview.window();
    CHECK(window != nullptr);
    CHECK(window->objectName() == QStringLiteral("preview-root"));
    CHECK(window->width() == 980);
    CHECK(window->height() == 700);

    QScreen* selected = QGuiApplication::primaryScreen();
    if (selected != nullptr && !selected->name().isEmpty()) {
        lapis::desktop::UiPreview placed(
            workspace,
            {.source = QUrl::fromLocalFile(qml_path), .compact = true, .screen = selected->name()});
        CHECK(placed.load());
        CHECK(placed.window()->screen() == selected);
#ifdef Q_OS_MACOS
        // macOS adjusts client geometry to leave room for native window decorations.
        CHECK(selected->availableGeometry().contains(placed.window()->frameGeometry()));
#else
        CHECK(selected->availableGeometry().contains(placed.window()->geometry()));
#endif
        placed.window()->hide();
    }

    lapis::desktop::UiPreview unmatched(workspace, {.source = QUrl::fromLocalFile(qml_path),
                                                    .compact = true,
                                                    .screen = QStringLiteral("no-such-screen")});
    CHECK(unmatched.load());
    CHECK(unmatched.diagnostics().contains(QStringLiteral("No screen matched")));
    unmatched.window()->hide();

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
void wait_active(QQuickWindow& window) {
    lapis::desktop::test::activate_test_window(window);
    QElapsedTimer elapsed;
    elapsed.start();
    while (!window.isActive() && elapsed.elapsed() < 5000)
        pump(10);
    if (!window.isActive())
        throw std::runtime_error("Window failed to activate: " + window.title().toStdString() +
                                 " size=" + std::to_string(window.width()) + "x" +
                                 std::to_string(window.height()));
}

void write_config(const QTemporaryDir& directory, const QString& text) {
    QFile file(directory.filePath(QStringLiteral("lapis.json")));
    CHECK(file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text));
    const auto bytes = text.toUtf8();
    CHECK(file.write(bytes) == bytes.size());
    CHECK(file.flush());
    file.close();
}

void wait_popup(QObject& dialog, bool open) {
    const auto* property = open ? "opened" : "visible";
    QElapsedTimer elapsed;
    elapsed.start();
    while (dialog.property(property).toBool() != open && elapsed.elapsed() < 5000)
        pump(10);
    CHECK(dialog.property(property).toBool() == open);
    pump(20); // Deliver queued focus restoration after the transition's completion signal.
}

void click_setting(QQuickWindow& window, const QString& name) {
    auto* item = find_visual(window.contentItem(), name);
    auto* scroll = find_visual(window.contentItem(), QStringLiteral("settingsScroll"));
    CHECK(item != nullptr && scroll != nullptr);
    scroll->setProperty("contentY", 0);
    pump(10);
    const auto offset = item->mapToItem(scroll, QPointF()).y();
    if (offset + item->height() > scroll->height()) {
        scroll->setProperty("contentY", offset + item->height() - scroll->height());
        pump(10);
    }
    const QPointF position = item->mapToScene(QPointF(item->width() / 2, item->height() / 2));
    const QPointF global = window.mapToGlobal(position);
    QMouseEvent press(QEvent::MouseButtonPress, position, global, Qt::LeftButton, Qt::LeftButton,
                      Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease, position, global, Qt::LeftButton, Qt::NoButton,
                        Qt::NoModifier);
    QCoreApplication::sendEvent(&window, &press);
    QCoreApplication::sendEvent(&window, &release);
    pump(50);
    CHECK(item->property("checked").toBool());
}

[[nodiscard]] QString read_text(const QString& path) {
    QFile file(path);
    CHECK(file.open(QIODevice::ReadOnly));
    const QByteArray contents = file.readAll();
    CHECK(!file.error());
    return QString::fromUtf8(contents);
}

int run_attention_dialog_tests() {
    using namespace lapis::desktop;
    namespace wire = lapis::session::wire;
    namespace attention = lapis::session::attention;
    Workspace workspace(WorkspaceMode::preview);
    UiPreview preview(workspace, {.source = QUrl::fromLocalFile(QStringLiteral(LAPIS_QML_SOURCE)),
                                  .compact = true,
                                  .screen = QString()});
    CHECK(preview.load());
    auto* window = preview.window();
    wait_active(*window);
    auto* terminal = find_visual(window->contentItem(), QStringLiteral("liveTerminal"));
    auto* dialog = window->findChild<QObject*>(QStringLiteral("attentionDialog"));
    CHECK(terminal != nullptr && dialog != nullptr);
    terminal->forceActiveFocus();
    auto* document = workspace.focusedSession();
    document->setConnection(QStringLiteral("ready"), true);
    wire::AttentionSnapshot state;
    state.attachment = {{wire::new_id(), wire::new_id()}, 1};
    state.available = state.connected = state.ready = true;
    state.source_epoch = 1;
    attention::Pending pending;
    pending.request = {.id = std::int64_t{1},
                       .thread_id = "fixture",
                       .turn_id = "turn",
                       .item_id = "item",
                       .reason = "User input",
                       .summary = "Test question",
                       .choices = {"submit"}};
    pending.source_epoch = pending.revision = 1;
    state.requests.push_back(
        {pending,
         QJsonObject{{"questions", QJsonArray{QJsonObject{{"id", "color"},
                                                          {"question", "Choose a color"},
                                                          {"options", QJsonValue::Null}}}}}});
    document->applyAttention(state);
    pump(20);
    CHECK(!dialog->property("visible").toBool());
    CHECK(terminal->hasActiveFocus());
    CHECK(QMetaObject::invokeMethod(window, "openAttentionDialog"));
    wait_popup(*dialog, true);
    CHECK(window->property("inputBlocked").toBool());
    CHECK(!preview.assignTerminalFocus());
    CHECK(window->activeFocusItem() != terminal);
    CHECK(QMetaObject::invokeMethod(dialog, "selectRequest",
                                    Q_ARG(QVariant, document->attentionRequests()[0])));
    pump(20);
    auto* answer = find_visual(window->contentItem(), QStringLiteral("answer-color"));
    CHECK(answer != nullptr);
    answer->forceActiveFocus();
    QKeyEvent letter(QEvent::KeyPress, Qt::Key_B, Qt::NoModifier, QStringLiteral("Blue"));
    QCoreApplication::sendEvent(window, &letter);
    CHECK(answer->property("text").toString() == QStringLiteral("Blue"));
    QInputMethodEvent compose(QStringLiteral("仮"), {});
    QCoreApplication::sendEvent(window, &compose);
    CHECK(answer->property("preeditText").toString() == QStringLiteral("仮"));
    state.diagnostic = QStringLiteral("Another request arrived");
    auto other = state.requests[0];
    other.pending.request.id = std::string("other");
    other.pending.revision = 2;
    state.requests.push_back(other);
    document->applyAttention(state);
    pump(20);
    CHECK(answer == find_visual(window->contentItem(), QStringLiteral("answer-color")));
    CHECK(answer->hasActiveFocus());
    CHECK(answer->property("text").toString() == QStringLiteral("Blue"));
    CHECK(answer->property("preeditText").toString() == QStringLiteral("仮"));
    QInputMethodEvent cancel;
    QCoreApplication::sendEvent(window, &cancel);
    QKeyEvent navigate(QEvent::KeyPress, Qt::Key_Tab, Qt::ControlModifier);
    QCoreApplication::sendEvent(window, &navigate);
    CHECK(workspace.focusedIndex() == 0);
    CHECK(preview.openSettings());
    CHECK(!window->findChild<QObject*>(QStringLiteral("settingsDialog"))
               ->property("visible")
               .toBool());
    CHECK(dialog->property("canRespond").toBool());
    document->invalidateAttention();
    CHECK(!dialog->property("canRespond").toBool());
    CHECK(answer->property("text").toString() == QStringLiteral("Blue"));
    if (const auto path = qEnvironmentVariable("LAPIS_ATTENTION_CAPTURE"); !path.isEmpty())
        CHECK(window->grabWindow().save(path));
    CHECK(QMetaObject::invokeMethod(dialog, "close"));
    wait_popup(*dialog, false);
    CHECK(!window->property("inputBlocked").toBool());
    CHECK(terminal->hasActiveFocus());
    return EXIT_SUCCESS;
}

int run_shortcut_focus_tests() {
    using namespace lapis::desktop;
    Workspace workspace(WorkspaceMode::preview);
    QTemporaryDir directory;
    CHECK(directory.isValid());
    write_config(directory, QStringLiteral(R"({"version":1,"layout":"focus","theme":"lapis",)"
                                           R"("keybindings":{"openSettings":["Ctrl+Alt+T"]}})"));
    KeyMap keymap;
    keymap.setSourcePathForTesting(directory.filePath(QStringLiteral("lapis.json")));
    CHECK(keymap.load());

    UiPreview preview(workspace, {.source = QUrl::fromLocalFile(QStringLiteral(LAPIS_QML_SOURCE)),
                                  .compact = true,
                                  .screen = QString(),
                                  .keymap = &keymap});
    CHECK(preview.load());
    auto* window = preview.window();
    window->show();
    window->requestActivate();
    wait_active(*window);
    auto* terminal = find_visual(window->contentItem(), QStringLiteral("liveTerminal"));
    auto* dialog = window->findChild<QObject*>(QStringLiteral("settingsDialog"));
    CHECK(terminal != nullptr && dialog != nullptr);
    terminal->forceActiveFocus();

    CHECK(QMetaObject::invokeMethod(dialog, "open"));
    wait_popup(*dialog, true);
    const int focused_at_modal = workspace.focusedIndex();
    const QString layout_at_modal = keymap.layoutName();
    const QString config_at_modal = read_text(directory.filePath(QStringLiteral("lapis.json")));
    int changes_during_modal = 0;
    const auto changed_connection = QObject::connect(
        &keymap, &KeyMap::changed, [&changes_during_modal] { ++changes_during_modal; });
    const QList<QPair<Qt::Key, Qt::KeyboardModifiers>> workspace_keys{
        {Qt::Key_Tab, Qt::ControlModifier},
        {Qt::Key_Tab, Qt::ControlModifier | Qt::ShiftModifier},
        {Qt::Key_1, Qt::ControlModifier},
        {Qt::Key_2, Qt::ControlModifier},
        {Qt::Key_3, Qt::ControlModifier},
        {Qt::Key_4, Qt::ControlModifier},
        {Qt::Key_Right, Qt::ControlModifier},
        {Qt::Key_L, Qt::ControlModifier},
        {Qt::Key_R, Qt::ControlModifier}};
    for (const auto& [key, modifiers] : workspace_keys) {
        QKeyEvent press(QEvent::KeyPress, key, modifiers);
        QKeyEvent release(QEvent::KeyRelease, key, modifiers);
        QCoreApplication::sendEvent(window, &press);
        QCoreApplication::sendEvent(window, &release);
        pump(10);
        CHECK(workspace.focusedIndex() == focused_at_modal);
        CHECK(keymap.layoutName() == layout_at_modal);
        CHECK(changes_during_modal == 0);
    }
    QObject::disconnect(changed_connection);
    CHECK(workspace.focusedIndex() == focused_at_modal);
    CHECK(keymap.layoutName() == layout_at_modal);
    CHECK(read_text(directory.filePath(QStringLiteral("lapis.json"))) == config_at_modal);
    CHECK(dialog->property("opened").toBool());
    CHECK(window->activeFocusItem() != terminal);
    CHECK(QMetaObject::invokeMethod(dialog, "close"));
    wait_popup(*dialog, false);
    CHECK(workspace.focusedIndex() == focused_at_modal);
    CHECK(keymap.layoutName() == layout_at_modal);

    QKeyEvent old_shortcut(QEvent::KeyPress, Qt::Key_Comma, Qt::ControlModifier);
    QCoreApplication::sendEvent(window, &old_shortcut);
    CHECK(!dialog->property("visible").toBool());
    QKeyEvent old_mac_shortcut(QEvent::KeyPress, Qt::Key_Comma, Qt::MetaModifier);
    QCoreApplication::sendEvent(window, &old_mac_shortcut);
    CHECK(!dialog->property("visible").toBool());

    QKeyEvent custom(QEvent::KeyPress, Qt::Key_T, Qt::ControlModifier | Qt::AltModifier,
                     QStringLiteral("t"));
    CHECK(QCoreApplication::sendEvent(window, &custom));
    CHECK(custom.isAccepted());
    wait_popup(*dialog, true);
    CHECK(dialog->property("opened").toBool());
    CHECK(window->activeFocusItem() != terminal);
    CHECK(dialog->property("shortcutHint").toString() == QStringLiteral("Ctrl+Alt+T"));
    if (const auto path = qEnvironmentVariable("LAPIS_SETTINGS_CAPTURE"); !path.isEmpty())
        CHECK(window->grabWindow().save(path));

    CHECK(keymap.setTheme(QStringLiteral("graphite")));
    pump(50);
    CHECK(dialog->property("opened").toBool());
    CHECK(window->activeFocusItem() != terminal);

    CHECK(keymap.setLayout(QStringLiteral("columns")));
    pump(50);
    CHECK(dialog->property("opened").toBool());
    CHECK(window->activeFocusItem() != terminal);

    CHECK(QMetaObject::invokeMethod(dialog, "close"));
    wait_popup(*dialog, false);
    CHECK(!dialog->property("opened").toBool());
    CHECK(terminal->hasActiveFocus());

    CHECK(preview.openSettings());
    wait_popup(*dialog, true);
    for (const auto& theme : {"lapis", "graphite", "daylight", "solarized", "amber", "contrast"}) {
        click_setting(*window, QStringLiteral("theme-") + QString::fromLatin1(theme));
        CHECK(keymap.themeName() == QString::fromLatin1(theme));
        CHECK(dialog->property("opened").toBool());
    }
    for (const auto& density : {"comfortable", "compact", "minimal"}) {
        click_setting(*window, QStringLiteral("choice-") + QString::fromLatin1(density));
        CHECK(keymap.densityName() == QString::fromLatin1(density));
    }
    click_setting(*window, QStringLiteral("theme-lapis"));
    for (const auto& layout : {"focus", "columns", "blocks", "stack"}) {
        click_setting(*window, QStringLiteral("choice-") + QString::fromLatin1(layout));
        CHECK(keymap.layoutName() == QString::fromLatin1(layout));
        CHECK(QMetaObject::invokeMethod(dialog, "close"));
        wait_popup(*dialog, false);
        auto* carousel = find_visual(window->contentItem(), QStringLiteral("sessionCarousel"));
        auto* shell_card = find_visual(window->contentItem(), QStringLiteral("sessionCard_shell"));
        CHECK(carousel != nullptr && shell_card != nullptr);
        if (keymap.layoutName() == QStringLiteral("stack")) {
            CHECK(shell_card->width() > carousel->width() * 0.95);
            CHECK(shell_card->height() > carousel->height() * 0.95);
            workspace.setFocusedIndex(1);
            pump(100);
            auto* next_card =
                find_visual(window->contentItem(), QStringLiteral("sessionCard_renderer"));
            CHECK(next_card != nullptr);
            CHECK(next_card->mapToItem(carousel, QPointF()).y() >= -1);
            CHECK(next_card->mapToItem(carousel, QPointF()).y() < carousel->height());
            workspace.setFocusedIndex(0);
            pump(100);
        }
        if (keymap.layoutName() == QStringLiteral("blocks")) {
            window->resize(700, 700);
            pump(100);
            CHECK(carousel->property("occupiedRows").toInt() >= 2);
            auto* last_card =
                find_visual(window->contentItem(), QStringLiteral("sessionCard_checks"));
            CHECK(last_card != nullptr);
            CHECK(last_card->mapToItem(carousel, QPointF()).y() > shell_card->height());
            CHECK(last_card->mapToItem(carousel, QPointF()).x() + last_card->width() <=
                  carousel->width());
            window->resize(980, 700);
            pump(100);
        }
        if (const auto path = qEnvironmentVariable("LAPIS_LAYOUT_CAPTURE_PREFIX"); !path.isEmpty())
            CHECK(window->grabWindow().save(path + QString::fromLatin1(layout) + ".png"));
        CHECK(preview.openSettings());
        wait_popup(*dialog, true);
    }
    KeyMap persisted;
    persisted.setSourcePathForTesting(directory.filePath(QStringLiteral("lapis.json")));
    CHECK(persisted.load());
    CHECK(persisted.layoutName() == keymap.layoutName());
    CHECK(persisted.themeName() == keymap.themeName());
    CHECK(persisted.densityName() == keymap.densityName());
    CHECK(QMetaObject::invokeMethod(dialog, "close"));
    wait_popup(*dialog, false);

    auto* shortcut = window->findChild<QObject*>(QStringLiteral("nextCategoryShortcut"));
    CHECK(shortcut != nullptr);
    const auto old_sequences = shortcut->property("sequences");
    write_config(directory, QStringLiteral(R"({"keybindings":{"nextCategory":["Ctrl+Alt+Y"]}})"));
    CHECK(keymap.reload());
    pump(50);
    CHECK(shortcut->property("sequences") != old_sequences);
    wait_active(*window);
    QKeyEvent obsolete(QEvent::KeyPress, Qt::Key_T, Qt::ControlModifier | Qt::AltModifier);
    QCoreApplication::sendEvent(window, &obsolete);
    CHECK(!dialog->property("visible").toBool());
    for (const auto modifier : {Qt::ControlModifier, Qt::MetaModifier}) {
        QKeyEvent settings_key(QEvent::KeyPress, Qt::Key_Comma, modifier);
        QCoreApplication::sendEvent(window, &settings_key);
        wait_popup(*dialog, true);
        CHECK(dialog->property("opened").toBool());
        CHECK(QMetaObject::invokeMethod(dialog, "close"));
        wait_popup(*dialog, false);
        CHECK(!dialog->property("visible").toBool());
    }

    // Preview documents are intentionally noninteractive. Enable only this
    // fixture item to exercise the real card's focus routing in the same window.
    CHECK(keymap.setLayout(QStringLiteral("stack")));
    pump(50);
    auto* card = find_visual(window->contentItem(), QStringLiteral("cardTerminal_shell"));
    CHECK(card != nullptr);
    card->setEnabled(true);
    wait_active(*window);
    preview.deferTerminalFocus();
    pump(50);
    if (!card->hasActiveFocus())
        std::cerr << "card focus: enabled=" << card->isEnabled() << " visible=" << card->isVisible()
                  << " window=" << window->isActive()
                  << " dialog=" << dialog->property("visible").toBool()
                  << " focused=" << workspace.focusedSession()->sessionId().toStdString() << '\n';
    CHECK(card->hasActiveFocus());
    return EXIT_SUCCESS;
}
struct ColoredArea {
    QRect bounds;
    int pixels{};
};
ColoredArea colored_area(const QImage& image, QRgb color) {
    ColoredArea area;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (image.pixel(x, y) != color)
                continue;
            area.bounds = area.bounds.united(QRect(x, y, 1, 1));
            ++area.pixels;
        }
    }
    return area;
}
void check_cursor_rendering(lapis::desktop::SessionPreview& document, QQuickWindow& window) {
    using namespace lapis::session;
    TerminalSnapshot snapshot;
    snapshot.size = {4, 2};
    snapshot.cells.resize(8);
    snapshot.background_rgb = 0x112233U;
    snapshot.foreground_rgb = 0x445566U;
    snapshot.cursor_rgb = 0xff0000U;
    snapshot.cursor = {.column = 1, .row = 0, .in_viewport = true, .visible = true};
    const auto capture = [&](CursorShape shape, QRgb expected = qRgb(255, 0, 0)) {
        snapshot.cursor.shape = shape;
        ++snapshot.revision;
        document.applySnapshot(snapshot);
        pump(150); // Include the bounded noninteractive preview refresh and presentation.
        const auto image = window.grabWindow();
        CHECK(!image.isNull());
        return colored_area(image, expected);
    };
    const auto block = capture(CursorShape::block);
    const auto bar = capture(CursorShape::bar);
    const auto underline = capture(CursorShape::underline);
    const auto hollow = capture(CursorShape::hollow_block);
    CHECK(block.pixels > 0 && bar.pixels > 0 && underline.pixels > 0 && hollow.pixels > 0);
    CHECK(bar.bounds.width() < block.bounds.width() / 2);
    CHECK(underline.bounds.height() < block.bounds.height() / 2);
    CHECK(hollow.bounds == block.bounds && hollow.pixels < block.pixels);
    snapshot.graphemes = U"X";
    snapshot.cells[1].text_length = 1;
    const auto with_glyph = capture(CursorShape::block);
    CHECK(with_glyph.bounds == block.bounds && with_glyph.pixels < block.pixels);
    snapshot.cells[1].text_length = 0;
    snapshot.cells[1].kind = CellKind::wide;
    snapshot.cells[2].kind = CellKind::wide_tail;
    const auto wide = capture(CursorShape::block);
    CHECK(wide.bounds.width() >= 2 * block.bounds.width() - 1);
    snapshot.cursor_rgb.reset();
    CHECK(capture(CursorShape::block, qRgb(0x44, 0x55, 0x66)).pixels > 0);
    snapshot.cursor.visible = false;
    CHECK(capture(CursorShape::block, qRgb(0x44, 0x55, 0x66)).pixels == 0);
    snapshot.cursor.visible = true;
    snapshot.cursor.in_viewport = false;
    CHECK(capture(CursorShape::block, qRgb(0x44, 0x55, 0x66)).pixels == 0);
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
    const auto original = document->snapshot();
    check_cursor_rendering(*document, window);
    document->applySnapshot(original);
    pump(30);
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
    UiPreview preview(workspace, {.source = QUrl::fromLocalFile(QStringLiteral(LAPIS_QML_SOURCE)),
                                  .compact = false,
                                  .screen = QString()});
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
    // Instrumentation can delay animation ticks. Check finite completion,
    // not a release-performance deadline inside a sanitizer test.
    QElapsedTimer completion;
    completion.start();
    while (card->property("cueRunning").toBool() && completion.elapsed() < 5000)
        pump(10);
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
    window->hide();
    pump(20); // Reduced motion must also clear a pulse paused by inactivity.
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

int run_input_guard_tests() {
    using namespace lapis::desktop;
    Workspace workspace(WorkspaceMode::preview);
    UiPreview preview(workspace, {.source = QUrl::fromLocalFile(QStringLiteral(LAPIS_QML_SOURCE)),
                                  .compact = true,
                                  .screen = QString()});
    CHECK(preview.load());
    auto* window = preview.window();
    wait_active(*window);
    auto* terminal = find_visual(window->contentItem(), QStringLiteral("liveTerminal"));
    CHECK(terminal != nullptr);
    terminal->forceActiveFocus();
    pump(20);

    const auto send_key = [&window](QEvent::Type type, int key, const QString& text = {},
                                    bool repeat = false) {
        QKeyEvent event(type, key, Qt::NoModifier, text, repeat, repeat ? 1 : 0);
        QCoreApplication::sendEvent(window, &event);
    };
    send_key(QEvent::KeyPress, Qt::Key_X, QStringLiteral("x"));
    send_key(QEvent::KeyPress, Qt::Key_X, QString(), true);
    CHECK(preview.holdingKeys());
    workspace.setFocusedIndex(1);
    pump(20);
    CHECK(workspace.focusedIndex() == 0);
    send_key(QEvent::KeyRelease, Qt::Key_X, QString(), true);
    CHECK(preview.holdingKeys());
    send_key(QEvent::KeyRelease, Qt::Key_X);
    CHECK(!preview.holdingKeys());
    pump(50);
    CHECK(workspace.focusedIndex() == 1);

    wait_active(*window);
    send_key(QEvent::KeyPress, Qt::Key_Shift);
    CHECK(preview.holdingKeys());
    workspace.setFocusedIndex(0);
    CHECK(workspace.focusedIndex() == 1);
    send_key(QEvent::KeyRelease, Qt::Key_Shift);
    pump(50);
    CHECK(workspace.focusedIndex() == 0);

    auto* dialog = window->findChild<QObject*>(QStringLiteral("sessionDialog"));
    CHECK(dialog != nullptr);
    CHECK(QMetaObject::invokeMethod(dialog, "open"));
    wait_popup(*dialog, true);
    workspace.setFocusedIndex(1);
    pump(20);
    CHECK(workspace.focusedIndex() == 0);
    CHECK(QMetaObject::invokeMethod(dialog, "close"));
    wait_popup(*dialog, false);
    pump(50);
    CHECK(workspace.focusedIndex() == 1);
    return EXIT_SUCCESS;
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication::setAttribute(Qt::AA_MacDontSwapCtrlAndMeta);
    QGuiApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Vulkan);
    QCoreApplication::setApplicationName(QStringLiteral("lapis-ui-preview-tests"));
    QCoreApplication::setOrganizationName(QStringLiteral("lapis"));
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    qmlRegisterUncreatableType<lapis::desktop::SessionPreview>("Lapis", 1, 0, "SessionPreview",
                                                               "Owned by workspace");
    qmlRegisterType<lapis::desktop::TerminalSurface>("Lapis", 1, 0, "TerminalSurface");
    try {
        if (app.arguments().contains(QStringLiteral("--shortcuts-only")))
            return run_shortcut_focus_tests();
        if (run_workspace_tests() != EXIT_SUCCESS || run_ui_tests() != EXIT_SUCCESS ||
            run_surface_tests() != EXIT_SUCCESS || run_attention_dialog_tests() != EXIT_SUCCESS ||
            run_attention_ui_tests() != EXIT_SUCCESS || run_input_guard_tests() != EXIT_SUCCESS)
            return EXIT_FAILURE;
        std::cout << "ui_preview_test: PASS\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "ui_preview_test: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
