#include "agent_search.hpp"
#include "keymap.hpp"
#include "platform/window_activation.hpp"
#include "terminal_surface.hpp"
#include "ui_preview.hpp"
#include "usage.hpp"
#include <QAccessible>
#include <QElapsedTimer>
#include <QHash>
#include <QJSValue>
#include <QQmlEngine>
#include <QQmlProperty>
#include <QQuickStyle>
#include <QSet>
#include <QThread>

#include <QClipboard>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QInputMethodEvent>
#include <QJsonArray>
#include <QKeyEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QObject>
#include <QPointer>
#include <QQuickWindow>
#include <QRect>
#include <QSGRendererInterface>
#include <QScopeGuard>
#include <QScreen>
#include <QTemporaryDir>
#include <QUrl>
#include <memory>

#include <array>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

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
    const int session_count = static_cast<int>(workspace.sessions().size());
    workspace.nextSession(-1);
    CHECK(workspace.focusedIndex() == session_count - 1);
    workspace.nextSession();
    CHECK(workspace.focusedIndex() == 0);
    workspace.nextSession(session_count + 2);
    CHECK(workspace.focusedIndex() == 2);
    workspace.nextSession(-2);
    CHECK(workspace.focusedIndex() == 0);

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
    // Appearance buttons are not checkable; callers verify the actual KeyMap selection.
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
    CHECK(!preview.openSettings());
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

// Appearance font controls apply to the live surface immediately, change the
// requested cell grid once per step, persist, and roll back on a failed save.
void check_terminal_font_controls(QQuickWindow& window, lapis::desktop::KeyMap& keymap,
                                  lapis::desktop::TerminalSurface& terminal,
                                  const QStringList& families, const QTemporaryDir& directory) {
    using lapis::desktop::kTerminalFontSizeDefault;
    CHECK(keymap.terminalFontSize() == kTerminalFontSizeDefault);
    CHECK(terminal.fontPixelSize() == kTerminalFontSizeDefault);
    // Snapshot the fallback: a reference would follow later family changes.
    // NOLINTNEXTLINE(performance-unnecessary-copy-initialization)
    const QString system_family = terminal.resolvedFontFamily();
    CHECK(!system_family.isEmpty());
    CHECK(window.property("monoFamily").toString() == system_family);
    const QSize base_grid = terminal.gridSize();
    CHECK(base_grid.width() > 2 && base_grid.height() > 2);

    int grid_changes = 0;
    const auto grid_connection =
        QObject::connect(&terminal, &lapis::desktop::TerminalSurface::gridSizeChanged,
                         [&grid_changes] { ++grid_changes; });
    const auto disconnect_grid =
        qScopeGuard([grid_connection] { QObject::disconnect(grid_connection); });
    click_setting(window, QStringLiteral("fontLarger"));
    CHECK(keymap.terminalFontSize() == kTerminalFontSizeDefault + 1);
    CHECK(terminal.fontPixelSize() == kTerminalFontSizeDefault + 1);
    CHECK(terminal.gridSize().width() < base_grid.width());
    CHECK(grid_changes == 1);
    click_setting(window, QStringLiteral("fontSmaller"));
    CHECK(keymap.terminalFontSize() == kTerminalFontSizeDefault);
    CHECK(terminal.gridSize() == base_grid);

    // A missing family falls back to the system fixed-width face and says so.
    CHECK(keymap.setTerminalFontFamily(QStringLiteral("lapis missing mono")));
    pump(30);
    CHECK(terminal.resolvedFontFamily() == system_family);
    auto* unavailable = find_visual(window.contentItem(), QStringLiteral("fontUnavailable"));
    CHECK(unavailable != nullptr && unavailable->isVisible());
    if (!families.isEmpty()) {
        CHECK(keymap.setTerminalFontFamily(families.front()));
        pump(30);
        CHECK(terminal.resolvedFontFamily() == families.front());
        CHECK(window.property("monoFamily").toString() == families.front());
        CHECK(!unavailable->isVisible());
    }
    CHECK(keymap.setTerminalFontFamily(QString()));
    pump(30);
    CHECK(terminal.resolvedFontFamily() == system_family);

    const QString config = directory.filePath(QStringLiteral("lapis.json"));
    const QString saved = read_text(config);
    write_config(directory, QStringLiteral("{unfinished"));
    const int changes_before_failure = grid_changes;
    click_setting(window, QStringLiteral("fontLarger"));
    CHECK(grid_changes == changes_before_failure); // No tentative resize on a failed save.
    CHECK(keymap.terminalFontSize() == kTerminalFontSizeDefault);
    CHECK(terminal.fontPixelSize() == kTerminalFontSizeDefault);
    CHECK(terminal.gridSize() == base_grid);
    CHECK(keymap.diagnostic().contains(QStringLiteral("Could not save")));
    write_config(directory, saved);
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
    for (const auto* action :
         {"nextWindow", "previousWindow", "nextCategory", "previousCategory", "category1",
          "category2", "newAgent", "newCategory", "reloadConfig"}) {
        for (const auto& binding : keymap.sequences(QString::fromLatin1(action))) {
            const auto combination = QKeySequence(binding)[0];
            QKeyEvent press(QEvent::KeyPress, combination.key(), combination.keyboardModifiers());
            QKeyEvent release(QEvent::KeyRelease, combination.key(),
                              combination.keyboardModifiers());
            QCoreApplication::sendEvent(window, &press);
            QCoreApplication::sendEvent(window, &release);
            pump(10);
            CHECK(workspace.focusedIndex() == focused_at_modal);
            CHECK(keymap.layoutName() == layout_at_modal);
            CHECK(changes_during_modal == 0);
        }
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
#ifdef Q_OS_MACOS
    CHECK(dialog->property("shortcutHint").toString() == QStringLiteral("⌃⌥T"));
#else
    CHECK(dialog->property("shortcutHint").toString() == QStringLiteral("Ctrl+Alt+T"));
#endif
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
    for (const auto& theme :
         {"lapis", "graphite", "daylight", "solarized", "amber", "contrast", "oled"}) {
        click_setting(*window, QStringLiteral("theme-") + QString::fromLatin1(theme));
        CHECK(keymap.themeName() == QString::fromLatin1(theme));
        CHECK(dialog->property("opened").toBool());
    }
    for (const auto& density : {"comfortable", "compact", "minimal"}) {
        click_setting(*window, QStringLiteral("choice-") + QString::fromLatin1(density));
        CHECK(keymap.densityName() == QString::fromLatin1(density));
    }
    click_setting(*window, QStringLiteral("theme-lapis"));
    check_terminal_font_controls(*window, keymap, *qobject_cast<TerminalSurface*>(terminal),
                                 preview.monospaceFamilies(), directory);
    KeyMap persisted;
    persisted.setSourcePathForTesting(directory.filePath(QStringLiteral("lapis.json")));
    CHECK(persisted.load());
    CHECK(persisted.layoutName() == keymap.layoutName());
    CHECK(persisted.themeName() == keymap.themeName());
    CHECK(persisted.densityName() == keymap.densityName());
    CHECK(persisted.terminalFontSize() == keymap.terminalFontSize());
    CHECK(persisted.terminalFontFamily() == keymap.terminalFontFamily());
    CHECK(QMetaObject::invokeMethod(dialog, "close"));
    wait_popup(*dialog, false);

    write_config(directory, QStringLiteral(R"({"keybindings":{"nextCategory":["Ctrl+Alt+Y"]}})"));
    CHECK(keymap.reload());
    pump(50);
    wait_active(*window);
    QKeyEvent obsolete(QEvent::KeyPress, Qt::Key_T, Qt::ControlModifier | Qt::AltModifier);
    QCoreApplication::sendEvent(window, &obsolete);
    CHECK(!dialog->property("visible").toBool());
    for (const auto& text : default_settings_shortcuts()) {
        const QKeySequence sequence(text);
        const auto combination = sequence[0];
        QKeyEvent settings_key(QEvent::KeyPress, combination.key(),
                               combination.keyboardModifiers());
        QCoreApplication::sendEvent(window, &settings_key);
        wait_popup(*dialog, true);
        CHECK(QMetaObject::invokeMethod(dialog, "close"));
        wait_popup(*dialog, false);
    }
#ifndef Q_OS_MACOS
    // X11 delivers Ctrl+Shift+, as Ctrl+Shift+< (observed on Linux).
    QKeyEvent shifted_settings(QEvent::KeyPress, Qt::Key_Less,
                               Qt::ControlModifier | Qt::ShiftModifier);
    QCoreApplication::sendEvent(window, &shifted_settings);
    wait_popup(*dialog, true);
    CHECK(QMetaObject::invokeMethod(dialog, "close"));
    wait_popup(*dialog, false);
#endif
    preview.deferTerminalFocus();
    pump(50);
    CHECK(terminal->hasActiveFocus());
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
        pump(30);
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

// Stage surfaces only: preview cards are scaled copies, not terminals.
bool preview_surface(const QQuickItem* item) {
    return item->objectName().startsWith(QStringLiteral("previewTerminal_"));
}
int surface_count(QQuickItem* item) {
    int count =
        qobject_cast<lapis::desktop::TerminalSurface*>(item) && !preview_surface(item) ? 1 : 0;
    for (auto* child : item->childItems())
        count += surface_count(child);
    return count;
}
void check_stage_bounds(QQuickWindow& window, QQuickItem& terminal) {
    const auto bounds = terminal.mapRectToScene(QRectF(QPointF{}, terminal.size()));
    CHECK(bounds.width() >= 100 && bounds.height() >= 100);
    CHECK(bounds.left() >= 0 && bounds.top() >= 0);
    CHECK(bounds.right() <= window.width() + 1 && bounds.bottom() <= window.height() + 1);
    CHECK(surface_count(window.contentItem()) == 1);
}
void send_binding(QQuickWindow& window, const QString& binding) {
    const auto combination = QKeySequence(binding)[0];
    QKeyEvent press(QEvent::KeyPress, combination.key(), combination.keyboardModifiers());
    QKeyEvent release(QEvent::KeyRelease, combination.key(), combination.keyboardModifiers());
    QCoreApplication::sendEvent(&window, &press);
    QCoreApplication::sendEvent(&window, &release);
}
void click_visual(QQuickWindow& window, QQuickItem& item) {
    const auto position = item.mapToScene(QPointF(item.width() / 2, item.height() / 2));
    CHECK(position.x() >= 0 && position.x() <= window.width());
    CHECK(position.y() >= 0 && position.y() <= window.height());
    const auto global = window.mapToGlobal(position);
    QMouseEvent press(QEvent::MouseButtonPress, position, global, Qt::LeftButton, Qt::LeftButton,
                      Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease, position, global, Qt::LeftButton, Qt::NoButton,
                        Qt::NoModifier);
    QCoreApplication::sendEvent(&window, &press);
    QCoreApplication::sendEvent(&window, &release);
}
void check_composition_navigation(lapis::desktop::Workspace& workspace,
                                  lapis::desktop::UiPreview& preview,
                                  lapis::desktop::TerminalSurface& terminal) {
    using namespace lapis::desktop;
    QTemporaryDir directory;
    CHECK(directory.isValid());
    auto* document = workspace.focusedSession();
    // A reconnect-only fixture owns no child. Inject ready state after the failed
    // reconnect to exercise actual Qt composition ownership, not CLI behavior.
    document->startLive(directory.filePath(QStringLiteral("absent.sock")),
                        {QStringLiteral("/bin/true"), {}, directory.path()},
                        lapis::session::wire::AttachMode::reconnect);
    pump(20);
    document->setConnection(QStringLiteral("ready"), true);
    document->applySnapshot(document->snapshot());
    terminal.setInteractive(true);
    terminal.forceActiveFocus();
    QInputMethodEvent composition(QStringLiteral("仮"), {});
    QCoreApplication::sendEvent(&terminal, &composition);
    CHECK(terminal.composing());
    // Snapshot the old value: a reference would hide a forbidden category change.
    // NOLINTNEXTLINE(performance-unnecessary-copy-initialization)
    const auto category = workspace.activeCategoryId();
    KeyMap defaults;
    for (const auto* action :
         {"nextWindow", "previousWindow", "nextCategory", "previousCategory", "category1",
          "category2", "newAgent", "newCategory", "openCommands", "toggleSidebar"})
        for (const auto& binding : defaults.sequences(QString::fromLatin1(action))) {
            send_binding(*preview.window(), binding);
            CHECK(workspace.focusedSession() == document);
            CHECK(workspace.activeCategoryId() == category);
            CHECK(terminal.composing());
        }
    CHECK(!preview.openSettings());
    const auto other_category =
        workspace.categories()[1].toMap().value(QStringLiteral("id")).toString();
    for (const auto& name :
         {QStringLiteral("agentTab_shell"), QStringLiteral("category_") + other_category,
          QStringLiteral("newAgentButton"), QStringLiteral("newCategoryButton"),
          QStringLiteral("commandsButton")}) {
        if (auto* strip = find_visual(preview.window()->contentItem(), QStringLiteral("agentTabs")))
            CHECK(QMetaObject::invokeMethod(strip, name == QStringLiteral("newAgentButton")
                                                       ? "positionViewAtEnd"
                                                       : "positionViewAtBeginning"));
        pump(20);
        auto* item = find_visual(preview.window()->contentItem(), name);
        CHECK(item != nullptr && item->isVisible());
        click_visual(*preview.window(), *item);
        CHECK(workspace.focusedSession() == document);
        CHECK(workspace.activeCategoryId() == category);
        CHECK(terminal.composing());
        CHECK(terminal.hasActiveFocus());
        CHECK(!preview.window()->property("inputBlocked").toBool());
    }
    QInputMethodEvent cancel;
    QCoreApplication::sendEvent(&terminal, &cancel);
    CHECK(!terminal.composing());
    preview.window()->resize(640, 480);
    pump(50);
    terminal.forceActiveFocus();
    QCoreApplication::sendEvent(&terminal, &composition);
    CHECK(terminal.composing());
    auto* selector =
        find_visual(preview.window()->contentItem(), QStringLiteral("categorySelector"));
    CHECK(selector != nullptr && selector->isVisible());
    click_visual(*preview.window(), *selector);
    CHECK(workspace.focusedSession() == document);
    CHECK(workspace.activeCategoryId() == category);
    CHECK(terminal.composing() && terminal.hasActiveFocus());
    CHECK(!preview.window()->property("inputBlocked").toBool());
    QCoreApplication::sendEvent(&terminal, &cancel);
    CHECK(!terminal.composing());

    auto* clipboard = QGuiApplication::clipboard();
    auto saved_clipboard = std::make_unique<QMimeData>();
    if (const auto* contents = clipboard->mimeData())
        for (const auto& format : contents->formats())
            saved_clipboard->setData(format, contents->data(format));
    const auto restore_clipboard =
        qScopeGuard([&] { clipboard->setMimeData(saved_clipboard.release()); });
    clipboard->setText(QStringLiteral("workspace paste guard"));
    bool observed_paste = false;
    const auto guard_connection =
        QObject::connect(&terminal, &TerminalSurface::inputOwnershipChanged, preview.window(), [&] {
            if (!terminal.pasting() || observed_paste)
                return;
            observed_paste = true;
            CHECK(!preview.window()->property("shortcutsArmed").toBool());
            CHECK(!preview.openSettings());
            CHECK(QMetaObject::invokeMethod(preview.window(), "openNewAgentDialog"));
            CHECK(!preview.window()->property("inputBlocked").toBool());
            const auto binding = defaults.sequences(QStringLiteral("nextCategory")).front();
            send_binding(*preview.window(), binding);
            CHECK(workspace.focusedSession() == document);
            CHECK(workspace.activeCategoryId() == category);
        });
    const auto disconnect_guard =
        qScopeGuard([guard_connection] { QObject::disconnect(guard_connection); });
    document->setConnection(QStringLiteral("ready"), true);
    terminal.forceActiveFocus();
    const auto paste_binding = QKeySequence(QKeySequence::Paste)[0];
    QKeyEvent paste(QEvent::KeyPress, paste_binding.key(), paste_binding.keyboardModifiers());
    QCoreApplication::sendEvent(&terminal, &paste);
    CHECK(observed_paste && !terminal.pasting());
}
QVariant call_window(QQuickWindow& window, const char* function, const QString& argument) {
    QVariant result;
    CHECK(QMetaObject::invokeMethod(&window, function, Q_RETURN_ARG(QVariant, result),
                                    Q_ARG(QVariant, QVariant(argument))));
    return result;
}

// The strip keeps only visible cards; scroll one into view before inspecting it.
void reveal_card(QQuickWindow& window, const lapis::desktop::Workspace& workspace,
                 const QString& id) {
    auto* strip = find_visual(window.contentItem(), QStringLiteral("agentTabs"));
    CHECK(strip != nullptr);
    const auto list = workspace.categorySessions();
    for (qsizetype i = 0; i < list.size(); ++i)
        if (list[i].value<lapis::desktop::SessionPreview*>()->sessionId() == id)
            CHECK(QMetaObject::invokeMethod(strip, "positionViewAtIndex", Q_ARG(int, int(i)),
                                            Q_ARG(int, 2 /* ListView.Contain */)));
    pump(30);
}

// Keyboard focus, activity, pending requests and lost connections stay visually
// distinct, and each status class has a non-color shape cue on its tab.
void check_status_semantics(QQuickWindow& window, lapis::desktop::Workspace& workspace) {
    const QStringList kinds{QStringLiteral("working"),    QStringLiteral("waiting"),
                            QStringLiteral("idle"),       QStringLiteral("finished"),
                            QStringLiteral("connecting"), QStringLiteral("disconnected"),
                            QStringLiteral("ended"),      QStringLiteral("unknown")};
    QHash<QString, QString> shapes;
    QHash<QString, QColor> colors;
    for (const auto& kind : kinds) {
        shapes.insert(kind, call_window(window, "statusShape", kind).toString());
        colors.insert(kind, call_window(window, "statusColor", kind).value<QColor>());
    }
    const QSet<QString> classes{
        shapes.value(QStringLiteral("working")), shapes.value(QStringLiteral("waiting")),
        shapes.value(QStringLiteral("idle")),    shapes.value(QStringLiteral("disconnected")),
        shapes.value(QStringLiteral("ended")),   shapes.value(QStringLiteral("unknown"))};
    CHECK(classes.size() == 6);
    CHECK(shapes.value(QStringLiteral("idle")) == shapes.value(QStringLiteral("finished")));
    CHECK(shapes.value(QStringLiteral("connecting")) == shapes.value(QStringLiteral("unknown")));
    const auto focus = window.property("focusedBorderColor").value<QColor>();
    const auto attention = window.property("attentionColor").value<QColor>();
    CHECK(focus.isValid() && attention.isValid());
    CHECK(colors.value(QStringLiteral("working")) != focus);
    CHECK(colors.value(QStringLiteral("waiting")) == attention);
    for (const auto& kind : {QStringLiteral("disconnected"), QStringLiteral("ended")}) {
        CHECK(colors.value(kind) != attention);
        CHECK(colors.value(kind) != focus);
    }
    namespace wire = lapis::session::wire;
    wire::AttentionSnapshot working;
    working.available = working.connected = working.ready = true;
    working.activity = lapis::session::attention::Activity::working;
    workspace.session(QStringLiteral("checks"))->applyAttention(working);
    auto idle = working;
    idle.activity = lapis::session::attention::Activity::idle;
    workspace.session(QStringLiteral("notes"))->applyAttention(idle);
    pump(20);
    reveal_card(window, workspace, QStringLiteral("checks"));
    auto* working_mark = find_visual(window.contentItem(), QStringLiteral("statusMark_checks"));
    CHECK(working_mark != nullptr);
    CHECK(working_mark->property("shape").toString() == QStringLiteral("dot"));
    CHECK(working_mark->property("tone").value<QColor>() ==
          window.property("activityColor").value<QColor>());
    for (const auto& id :
         {QStringLiteral("agent"), QStringLiteral("shell"), QStringLiteral("notes")}) {
        reveal_card(window, workspace, id);
        auto* mark = find_visual(window.contentItem(), QStringLiteral("statusMark_") + id);
        CHECK(mark != nullptr && mark->isVisible());
        CHECK(mark->property("shape").toString() ==
              shapes.value(workspace.session(id)->statusKind()));
    }
    CHECK(find_visual(window.contentItem(), QStringLiteral("statusMark_agent"))
              ->property("shape")
              .toString() == QStringLiteral("diamond"));
}

void check_category_alignment(QQuickWindow& window) {
    auto* rail = find_visual(window.contentItem(), QStringLiteral("categoryRail"));
    auto* stage = find_visual(window.contentItem(), QStringLiteral("focusedPane"));
    CHECK(rail != nullptr && stage != nullptr);
    if (!rail->isVisible())
        return;
    auto* category = find_visual(window.contentItem(), QStringLiteral("category_general"));
    CHECK(category != nullptr);
    CHECK(qAbs(category->mapToItem(window.contentItem(), QPointF()).y() -
               stage->mapToItem(window.contentItem(), QPointF()).y()) <= 1);
}

void wait_tab_visible(QQuickItem& tab, QQuickItem& view) {
    const auto visible = [&] {
        const auto position = tab.mapToItem(&view, QPointF());
        return position.x() >= -1 && position.x() + tab.width() <= view.width() + 1;
    };
    QElapsedTimer deadline;
    deadline.start();
    while (!visible() && deadline.elapsed() < 2000)
        pump(5);
    CHECK(visible());
}

int run_attention_ui_tests() {
    using namespace lapis::desktop;
    Workspace workspace(WorkspaceMode::preview);
    QTemporaryDir palette_config;
    CHECK(palette_config.isValid());
    KeyMap keymap;
    keymap.setSourcePathForTesting(palette_config.filePath(QStringLiteral("palette.json")));
    UiPreview preview(workspace, {.source = QUrl::fromLocalFile(QStringLiteral(LAPIS_QML_SOURCE)),
                                  .compact = false,
                                  .screen = QString(),
                                  .keymap = &keymap});
    CHECK(preview.load());
    auto* window = preview.window();
    wait_active(*window);
    pump(50);
    auto* terminal = qobject_cast<TerminalSurface*>(
        find_visual(window->contentItem(), QStringLiteral("liveTerminal")));
    CHECK(terminal != nullptr);
    terminal->forceActiveFocus();
    auto* original = workspace.focusedSession();
    const auto stage_size = terminal->size();
    CHECK(workspace.replayAttention(QStringLiteral("two")));
    pump(50);
    check_status_semantics(*window, workspace);
    CHECK(workspace.focusedSession() == original);
    CHECK(terminal->document() == original);
    CHECK(terminal->hasActiveFocus());
    CHECK(terminal->size() == stage_size);
    CHECK(workspace.categories().front().toMap().value(QStringLiteral("attentionCount")).toInt() ==
          2);
    CHECK(!workspace.replayAttention(QStringLiteral("duplicate")));
    CHECK(workspace.replayAttention(QStringLiteral("resolve")));
    CHECK(workspace.session(QStringLiteral("renderer"))->attentionPending());
    CHECK(workspace.focusedSession() == original);
    preview.setReducedMotion(true);
    CHECK(workspace.replayAttention(QStringLiteral("arrival")));
    CHECK(workspace.focusedSession() == original);

    CHECK(workspace.selectSession(QStringLiteral("agent")));
    auto* selected = workspace.focusedSession();
    CHECK(workspace.addCategory(QStringLiteral("Research")));
    const auto research = workspace.activeCategoryId();
    pump(20);
    CHECK(workspace.focusedSession() == nullptr);
    CHECK(terminal->document() == nullptr);
    CHECK(workspace.moveSession(QStringLiteral("renderer"), research));
    pump(20);
    CHECK(terminal->document() == workspace.session(QStringLiteral("renderer")));
    workspace.nextSession();
    CHECK(workspace.focusedSession() == workspace.session(QStringLiteral("renderer")));
    CHECK(workspace.selectCategory(QStringLiteral("general")));
    pump(20);
    CHECK(workspace.focusedSession() == selected);
    CHECK(terminal->document() == selected);
    CHECK(workspace.moveSessionBy(selected->sessionId(), 1));
    pump(20);
    CHECK(terminal->document() == selected);
    CHECK(workspace.session(selected->sessionId()) == selected);
    auto* category_button =
        find_visual(window->contentItem(), QStringLiteral("category_") + research);
    CHECK(category_button != nullptr);
    click_visual(*window, *category_button);
    pump(20);
    CHECK(workspace.activeCategoryId() == research);
    CHECK(terminal->document() == workspace.session(QStringLiteral("renderer")));
    category_button = find_visual(window->contentItem(), QStringLiteral("category_general"));
    CHECK(category_button != nullptr);
    click_visual(*window, *category_button);
    pump(20);
    CHECK(workspace.focusedSession() == selected);
    auto* tab = find_visual(window->contentItem(), QStringLiteral("agentTab_shell"));
    CHECK(tab != nullptr);
    const auto original_tab_width = tab->width();
    click_visual(*window, *tab);
    pump(20);
    CHECK(workspace.focusedSession() == original);
    CHECK(tab->width() == original_tab_width);
    tab = find_visual(window->contentItem(), QStringLiteral("agentTab_agent"));
    CHECK(tab != nullptr);
    click_visual(*window, *tab);
    pump(20);
    CHECK(workspace.focusedSession() == selected);
    CHECK(workspace.renameSession(selected->sessionId(), QString(80, QLatin1Char('W'))));
    CHECK(workspace.renameCategory(QStringLiteral("general"), QString(80, QLatin1Char('W'))));
    struct ChromeVariant {
        const char* density;
        int font_size;
    };
    // Default chrome, then the smallest chrome with much larger terminal text.
    for (const auto variant :
         {ChromeVariant{"comfortable", lapis::desktop::kTerminalFontSizeDefault},
          ChromeVariant{"minimal", 24}}) {
        for (const auto& requested : {QSize(640, 480), QSize(700, 900), QSize(980, 700),
                                      QSize(1400, 960), QSize(2560, 1080)}) {
            // Native macOS windows are constrained to the screen work area.
            // Exercise the actual available layout rather than an impossible size.
            const auto margins = window->frameMargins();
            const auto available =
                window->screen()->availableGeometry().size() -
                QSize(margins.left() + margins.right(), margins.top() + margins.bottom());
            const auto size = requested.boundedTo(available);
            CHECK(keymap.setDensity(QString::fromLatin1(variant.density)));
            CHECK(keymap.setTerminalFontSize(variant.font_size));
            window->resize(size);
            pump(80);
            CHECK(terminal->fontPixelSize() == variant.font_size);
            CHECK(window->size() == size);
            check_stage_bounds(*window, *terminal);
            auto* rail = find_visual(window->contentItem(), QStringLiteral("categoryRail"));
            auto* selector = find_visual(window->contentItem(), QStringLiteral("categorySelector"));
            CHECK(rail != nullptr && selector != nullptr);
            CHECK(rail->isVisible() == (size.width() >= 860));
            CHECK(selector->isVisible() == (size.width() < 860));
            CHECK(terminal->document() == selected);
            auto* tabs = find_visual(window->contentItem(), QStringLiteral("agentTabs"));
            auto* selected_tab = find_visual(window->contentItem(),
                                             QStringLiteral("agentTab_") + selected->sessionId());
            CHECK(tabs != nullptr && selected_tab != nullptr);
            check_category_alignment(*window);
            wait_tab_visible(*selected_tab, *tabs);
            CHECK(preview.openSettings());
            auto* settings = window->findChild<QObject*>(QStringLiteral("settingsDialog"));
            CHECK(settings != nullptr);
            wait_popup(*settings, true);
            CHECK(settings->property("x").toReal() >= 0);
            CHECK(settings->property("y").toReal() >= 0);
            CHECK(settings->property("x").toReal() + settings->property("width").toReal() <=
                  window->width() + 1);
            CHECK(settings->property("y").toReal() + settings->property("height").toReal() <=
                  window->height() + 1);
            CHECK(QMetaObject::invokeMethod(settings, "close"));
            wait_popup(*settings, false);
            if (const auto path = qEnvironmentVariable("LAPIS_WORKSPACE_CAPTURE_PREFIX");
                !path.isEmpty())
                CHECK(window->grabWindow().save(
                    path + QString::number(size.width()) + QLatin1Char('x') +
                    QString::number(size.height()) +
                    (variant.font_size == lapis::desktop::kTerminalFontSizeDefault
                         ? QString()
                         : QStringLiteral("-") + QString::fromLatin1(variant.density) +
                               QStringLiteral("-font") + QString::number(variant.font_size)) +
                    QStringLiteral(".png")));
        }
    }
    CHECK(keymap.setDensity(QStringLiteral("comfortable")));
    CHECK(keymap.setTerminalFontSize(lapis::desktop::kTerminalFontSizeDefault));
    if (const auto path = qEnvironmentVariable("LAPIS_WORKSPACE_CAPTURE_PREFIX"); !path.isEmpty()) {
        // Visual review only: semantic colors across contrasting themes, and
        // Appearance with its font controls.
        window->resize(1400, 960);
        for (const auto* theme : {"daylight", "contrast", "amber", "oled", "lapis"}) {
            CHECK(keymap.setTheme(QString::fromLatin1(theme)));
            pump(80);
            CHECK(window->grabWindow().save(path + QStringLiteral("theme-") +
                                            QString::fromLatin1(theme) + QStringLiteral(".png")));
        }
        CHECK(preview.openSettings());
        auto* settings = window->findChild<QObject*>(QStringLiteral("settingsDialog"));
        wait_popup(*settings, true);
        CHECK(window->grabWindow().save(path + QStringLiteral("appearance.png")));
        CHECK(QMetaObject::invokeMethod(settings, "close"));
        wait_popup(*settings, false);
    }
    // Category/sidebar navigation does not replace the selected agent.
    window->resize(1400, 960);
    pump(40);
    send_binding(*window, keymap.sequences(QStringLiteral("toggleSidebar")).front());
    pump(30);
    CHECK(!keymap.sidebarVisible());
    CHECK(!find_visual(window->contentItem(), QStringLiteral("categoryRail"))->isVisible());
    CHECK(terminal->document() == selected);
    send_binding(*window, keymap.sequences(QStringLiteral("toggleSidebar")).front());
    pump(30);
    CHECK(keymap.sidebarVisible());
    auto* stage = find_visual(window->contentItem(), QStringLiteral("focusedPane"));
    CHECK(stage != nullptr);
    const auto stage_edge = [stage] {
        return QQmlProperty::read(stage, QStringLiteral("border.color")).value<QColor>();
    };
    CHECK(stage_edge() == window->property("focusedBorderColor").value<QColor>());
    send_binding(*window, keymap.sequences(QStringLiteral("openCommands")).front());
    auto* palette = window->findChild<QObject*>(QStringLiteral("commandsDialog"));
    CHECK(palette != nullptr);
    wait_popup(*palette, true);
    CHECK(window->property("inputBlocked").toBool());
    // The focus accent leaves the stage while Commands owns the keyboard.
    CHECK(stage_edge() == window->property("borderColor").value<QColor>());
    auto* search = find_visual(window->contentItem(), QStringLiteral("commandSearch"));
    auto* results = find_visual(window->contentItem(), QStringLiteral("commandResults"));
    CHECK(search != nullptr && results != nullptr);
    CHECK(search->hasActiveFocus());
    if (const auto path = qEnvironmentVariable("LAPIS_WORKSPACE_CAPTURE_PREFIX"); !path.isEmpty())
        CHECK(window->grabWindow().save(path + QStringLiteral("commands.png")));
    CHECK(search->setProperty("text", QStringLiteral("Reconnect agent")));
    pump(20);
    CHECK(results->property("count").toInt() == 1);
    CHECK(QMetaObject::invokeMethod(palette, "choose", Q_ARG(QVariant, QVariant(0))));
    CHECK(palette->property("visible").toBool()); // Disabled actions stay explanatory.
    CHECK(terminal->document() == selected);
    CHECK(search->setProperty("text", QStringLiteral("No such command xyz")));
    pump(20);
    CHECK(results->property("count").toInt() == 0);
    CHECK(search->setProperty("text", QStringLiteral("New category")));
    pump(20);
    CHECK(results->property("count").toInt() == 1);
    CHECK(QMetaObject::invokeMethod(palette, "choose", Q_ARG(QVariant, QVariant(0))));
    wait_popup(*palette, false);
    auto* category_dialog = window->findChild<QObject*>(QStringLiteral("categoryDialog"));
    CHECK(category_dialog != nullptr);
    wait_popup(*category_dialog, true);
    CHECK(QMetaObject::invokeMethod(category_dialog, "close"));
    wait_popup(*category_dialog, false);
    CHECK(terminal->document() == selected);
    // New-agent entry needs only a folder, and completion stays on the keyboard.
    CHECK(QMetaObject::invokeMethod(window, "openNewAgentDialog"));
    auto* new_agent = window->findChild<QObject*>(QStringLiteral("agentDialog"));
    CHECK(new_agent != nullptr);
    wait_popup(*new_agent, true);
    auto* harnesses = find_visual(window->contentItem(), QStringLiteral("harnessChoices"));
    CHECK(harnesses != nullptr && harnesses->hasActiveFocus());
    const auto mode = [](const char* id) {
        return QVariantMap{{QStringLiteral("id"), QString::fromLatin1(id)},
                           {QStringLiteral("name"), QString::fromLatin1(id)}};
    };
    const auto model = [](const char* id, const char* name, bool fallback) {
        return QVariantMap{{QStringLiteral("id"), QString::fromLatin1(id)},
                           {QStringLiteral("name"), QString::fromLatin1(name)},
                           {QStringLiteral("default"), fallback}};
    };
    const QVariantList choices{
        QVariantMap{{QStringLiteral("id"), QStringLiteral("codex")},
                    {QStringLiteral("name"), QStringLiteral("Codex")},
                    {QStringLiteral("installed"), true}},
        QVariantMap{
            {QStringLiteral("id"), QStringLiteral("claude")},
            {QStringLiteral("name"), QStringLiteral("Claude")},
            {QStringLiteral("installed"), true},
            {QStringLiteral("models"), QVariantList{model("opus", "Opus 5.5", true),
                                                    model("claude-fable-5-1", "Fable 5.1", false)}},
            {QStringLiteral("modes"), QVariantList{mode("edits"), mode("auto"), mode("full")}}},
        QVariantMap{{QStringLiteral("id"), QStringLiteral("omp")},
                    {QStringLiteral("name"), QStringLiteral("OMP")},
                    {QStringLiteral("installed"), true},
                    {QStringLiteral("modes"), QVariantList{mode("edits"), mode("full")}}}};
    CHECK(new_agent->setProperty("harnesses", choices));
    CHECK(harnesses->setProperty("currentIndex", 0));
    if (const auto path = qEnvironmentVariable("LAPIS_WORKSPACE_CAPTURE_PREFIX"); !path.isEmpty())
        CHECK(window->grabWindow().save(path + QStringLiteral("harnesses.png")));
    send_binding(*window, QStringLiteral("Down"));
    send_binding(*window, QStringLiteral("Return"));
    pump(30);
    CHECK(new_agent->property("selectedHarness").toString() == QStringLiteral("claude"));
    CHECK(new_agent->property("phase").toInt() == 1);
    // The mode and each CLI's model stay while the CLI changes: OMP has no
    // Auto, so it uses Accept edits, and Claude gets Auto and Fable back.
    const auto call = [&](const char* method, const QVariant& value) {
        CHECK(QMetaObject::invokeMethod(new_agent, method, Q_ARG(QVariant, value)));
        pump(20);
    };
    CHECK(new_agent->property("selectedMode").toString() == QStringLiteral("full"));
    call("chooseMode", QStringLiteral("auto"));
    call("chooseModel", QStringLiteral("claude-fable-5-1"));
    CHECK(new_agent->property("selectedMode").toString() == QStringLiteral("auto"));
    call("chooseHarness", 2);
    CHECK(new_agent->property("selectedMode").toString() == QStringLiteral("edits"));
    CHECK(!find_visual(window->contentItem(), QStringLiteral("mode_auto"))->isEnabled());
    call("chooseHarness", 1);
    CHECK(new_agent->property("selectedMode").toString() == QStringLiteral("auto"));
    CHECK(new_agent->property("selectedModel").toString() == QStringLiteral("claude-fable-5-1"));
    call("chooseMode", QStringLiteral("full"));
    call("chooseHarness", 2);
    CHECK(new_agent->property("selectedMode").toString() == QStringLiteral("full"));
    call("chooseHarness", 1);
    auto* folder = find_visual(window->contentItem(), QStringLiteral("agentDirectoryField"));
    CHECK(folder != nullptr && folder->hasActiveFocus());
    send_binding(*window, QStringLiteral("Esc"));
    pump(20);
    CHECK(new_agent->property("visible").toBool() && new_agent->property("phase").toInt() == 0);
    CHECK(harnesses->hasActiveFocus());
    send_binding(*window, QStringLiteral("Return"));
    pump(20);
    CHECK(folder->hasActiveFocus());
    CHECK(folder->property("text").toString() == workspace.homeDirectory() + QLatin1Char('/'));
    CHECK(window->findChild<QObject*>(QStringLiteral("agentTitleField")) == nullptr);
    QTemporaryDir projects;
    CHECK(projects.isValid());
    CHECK(QDir(projects.path()).mkdir(QStringLiteral("project one")));
    CHECK(folder->setProperty("text", projects.path() + QStringLiteral("/proj")));
    auto* folders = find_visual(window->contentItem(), QStringLiteral("folderResults"));
    CHECK(folders != nullptr);
    for (int attempt = 0; attempt < 100 && folders->property("count").toInt() != 1; ++attempt)
        pump(10);
    CHECK(folders->property("count").toInt() == 1);
    send_binding(*window, QStringLiteral("Down"));
    send_binding(*window, QStringLiteral("Tab"));
    pump(30);
    CHECK(folder->property("text").toString() == projects.path() + QStringLiteral("/project one/"));
    CHECK(folder->hasActiveFocus());
    if (const auto path = qEnvironmentVariable("LAPIS_WORKSPACE_CAPTURE_PREFIX"); !path.isEmpty())
        CHECK(window->grabWindow().save(path + QStringLiteral("new-agent.png")));
    CHECK(QMetaObject::invokeMethod(new_agent, "close"));
    wait_popup(*new_agent, false);
    CHECK(terminal->document() == selected);
    check_composition_navigation(workspace, preview, *terminal);
    const auto original_font = QGuiApplication::font();
    const auto restore_font =
        qScopeGuard([original_font] { QGuiApplication::setFont(original_font); });
    auto larger_font = original_font;
    larger_font.setPixelSize(20);
    QGuiApplication::setFont(larger_font);
    pump(80);
    check_stage_bounds(*window, *terminal);
    auto* menu_button = find_visual(window->contentItem(), QStringLiteral("commandsButton"));
    CHECK(menu_button != nullptr && menu_button->isVisible());
    click_visual(*window, *menu_button);
    auto* menu = window->findChild<QObject*>(QStringLiteral("commandsDialog"));
    CHECK(menu != nullptr);
    wait_popup(*menu, true);
    CHECK(menu->property("x").toReal() >= 0);
    CHECK(menu->property("y").toReal() >= 0);
    CHECK(menu->property("x").toReal() + menu->property("width").toReal() <= window->width() + 1);
    CHECK(menu->property("y").toReal() + menu->property("height").toReal() <= window->height() + 1);
    CHECK(QMetaObject::invokeMethod(menu, "close"));
    wait_popup(*menu, false);
    CHECK(preview.diagnostics().isEmpty());
    return EXIT_SUCCESS;
}

QString focused_id(const lapis::desktop::Workspace& workspace) {
    return workspace.focusedSession() ? workspace.focusedSession()->sessionId() : QString();
}
void finish_turn(lapis::desktop::SessionPreview& session) {
    namespace wire = lapis::session::wire;
    wire::AttentionSnapshot state;
    state.available = state.connected = state.ready = true;
    state.activity = lapis::session::attention::Activity::working;
    session.applyAttention(state);
    state.activity = lapis::session::attention::Activity::turn_completed;
    session.applyAttention(state);
}

QQuickItem* required_visual(QQuickWindow& window, const QString& name) {
    auto* found = find_visual(window.contentItem(), name);
    if (found == nullptr)
        throw std::runtime_error("missing item " + name.toStdString());
    return found;
}
void press_action(QQuickWindow& window, const lapis::desktop::KeyMap& keymap, const char* action) {
    send_binding(window, keymap.sequences(QString::fromLatin1(action)).front());
    pump(260); // Longer than the strip's scroll animation.
}
void capture_step(QQuickWindow& window, const char* name) {
    if (const auto path = qEnvironmentVariable("LAPIS_WORKSPACE_CAPTURE_PREFIX"); !path.isEmpty())
        CHECK(window.grabWindow().save(path + QString::fromLatin1(name) + QStringLiteral(".png")));
}

void check_agent_search(QQuickWindow& window, lapis::desktop::Workspace& workspace,
                        const lapis::desktop::KeyMap& keymap,
                        lapis::desktop::TerminalSurface& terminal) {
    // Command-K finds an agent by name; Return shows it and gives it the keys.
    CHECK(workspace.selectSession(QStringLiteral("shell")));
    pump(30);
    press_action(window, keymap, "searchAgents");
    auto* finder = window.findChild<QObject*>(QStringLiteral("searchDialog"));
    CHECK(finder != nullptr);
    wait_popup(*finder, true);
    auto* field = required_visual(window, QStringLiteral("agentSearchField"));
    CHECK(field->hasActiveFocus());
    field->setProperty("text", QStringLiteral("wsnotes"));
    pump(30);
    CHECK(required_visual(window, QStringLiteral("agentResult_notes")) != nullptr);
    send_binding(window, QStringLiteral("Return"));
    wait_popup(*finder, false);
    CHECK(focused_id(workspace) == QStringLiteral("notes") && terminal.hasActiveFocus());
    // Escape leaves the focused agent alone.
    press_action(window, keymap, "searchAgents");
    wait_popup(*finder, true);
    required_visual(window, QStringLiteral("agentSearchField"))
        ->setProperty("text", QStringLiteral("shell"));
    pump(30);
    send_binding(window, QStringLiteral("Escape"));
    wait_popup(*finder, false);
    CHECK(focused_id(workspace) == QStringLiteral("notes"));
}

void check_usage(QQuickWindow& window, lapis::desktop::Usage& usage, lapis::desktop::KeyMap& keymap,
                 lapis::desktop::TerminalSurface& terminal) {
    // Plan usage sits under the categories, each CLI at its tightest window,
    // and opens the details; the setting hides it.
    usage.refresh();
    QElapsedTimer asked;
    asked.start();
    const auto remote_ready = [&usage] {
        const auto machines = usage.machines();
        return machines.size() == 2 &&
               machines.back().toMap().value(QStringLiteral("providers")).toList().size() >= 2;
    };
    while ((usage.meter().size() < 2 || usage.counting() || !remote_ready()) &&
           asked.elapsed() < 15000)
        pump(20);
    pump(60);
    auto* meter = required_visual(window, QStringLiteral("usageMeter"));
    CHECK(meter->isVisible());
    CHECK(required_visual(window, QStringLiteral("usageMeter_codex")) != nullptr &&
          required_visual(window, QStringLiteral("usageMeter_claude")) != nullptr);
    CHECK(meter->mapToScene({0, 0}).y() >
          required_visual(window, QStringLiteral("newCategoryButton"))->mapToScene({0, 0}).y());
    // The meter reads what is left: 96% used is 4% left, red; 41% used is
    // 59% left, green, with the longer bar.
    auto* codex_left = required_visual(window, QStringLiteral("usageLeft_codex"));
    auto* claude_left = required_visual(window, QStringLiteral("usageLeft_claude"));
    CHECK(codex_left->property("text").toString() == QStringLiteral("4% left wk"));
    CHECK(claude_left->property("text").toString() == QStringLiteral("59% left 5h"));
    CHECK(codex_left->property("color").value<QColor>() ==
          window.property("scarceColor").value<QColor>());
    CHECK(claude_left->property("color").value<QColor>() ==
          window.property("plentyColor").value<QColor>());
    pump(260); // past the bars' motion
    CHECK(required_visual(window, QStringLiteral("usageBar_codex"))->width() <
          required_visual(window, QStringLiteral("usageBar_claude"))->width());
    capture_step(window, "usage-meter");
    click_visual(window, *meter);
    auto* details = window.findChild<QObject*>(QStringLiteral("usageDialog"));
    CHECK(details != nullptr);
    wait_popup(*details, true);
    CHECK(required_visual(window, QStringLiteral("usage_codex")) != nullptr &&
          required_visual(window, QStringLiteral("usage_claude")) != nullptr);
    CHECK(
        required_visual(window, QStringLiteral("usageToday_claude"))->property("text").toString() ==
        QStringLiteral("0"));
    capture_step(window, "usage-details");
    // Each configured machine has its own tab.
    click_visual(window, *required_visual(window, QStringLiteral("usageMachine_devbox")));
    pump(60);
    CHECK(details->property("machineIndex").toInt() == 1);
    CHECK(required_visual(window, QStringLiteral("usageAccount_codex_0")) != nullptr);
    capture_step(window, "usage-devbox");
    send_binding(window, QStringLiteral("Escape"));
    wait_popup(*details, false);
    CHECK(terminal.hasActiveFocus());
    CHECK(keymap.setShowUsage(false));
    pump(30);
    CHECK(!meter->isVisible());
}

// Stand-ins for the two CLIs' usage answers; no transcripts.
std::unique_ptr<lapis::desktop::Usage> fake_usage(const QTemporaryDir& config) {
    const auto fake = [&config](const QString& name, const QByteArray& body) {
        const auto path = config.filePath(name);
        QFile file(path);
        CHECK(file.open(QIODevice::WriteOnly) && file.write("#!/bin/sh\n" + body) > 0);
        file.close();
        CHECK(QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                              QFileDevice::ExeOwner));
        return path;
    };
    const auto codex_cli = fake(QStringLiteral("codex"), R"(read -r line
echo '{"id":1,"result":{}}'
read -r line
read -r line
echo '{"id":2,"result":{"rateLimits":{"limitId":"codex","primary":{"usedPercent":96,"windowDurationMins":10080,"resetsAt":4102444800},"planType":"pro"}}}'
cat >/dev/null
)");
    const auto claude_cli = fake(QStringLiteral("claude"), R"(read -r line
echo '{"type":"control_response","response":{"subtype":"success","request_id":"usage","response":{"subscription_type":"max","rate_limits_available":true,"rate_limits":{"five_hour":{"utilization":41,"resets_at":"2099-01-01T00:00:00+00:00"},"seven_day":{"utilization":23,"resets_at":"2099-01-01T00:00:00+00:00"}}}}}'
cat >/dev/null
)");
    // devbox: ssh runs the command here, where a login shell finds the same
    // stand-ins.
    const auto dir = config.path().toUtf8();
    const auto shell =
        fake(QStringLiteral("login-shell"), "for last; do :; done\nPATH='" + dir +
                                                R"(:/usr/bin:/bin' exec /bin/sh -c "$last")"
                                                "\n");
    const auto ssh = fake(QStringLiteral("ssh"), R"(while [ $# -gt 0 ]; do
  case "$1" in -T) shift ;; -o|-L) shift 2 ;; *) break ;; esac
done
shift
HOME=')" + dir + R"(' SHELL=')" + shell.toUtf8() + R"(' exec /bin/sh -c "$1"
)");
    auto usage = std::make_unique<lapis::desktop::Usage>(
        [codex_cli, claude_cli, ssh](const QString& id) {
            return id == QLatin1String("codex")    ? codex_cli
                   : id == QLatin1String("claude") ? claude_cli
                   : id == QLatin1String("ssh")    ? ssh
                                                   : QString();
        },
        lapis::desktop::TokenLedger::Roots{config.filePath(QStringLiteral("none")),
                                           config.filePath(QStringLiteral("none"))});
    usage->setMachines({QStringLiteral("devbox")});
    return usage;
}

// The agent strip is the category's navigation: live previews in tab order
// that never take input or resize a terminal, keep part of the neighboring
// card in view as selection moves, and pulse an agent that finished or needs a
// response until it is selected.
int run_strip_ui_tests() {
    using namespace lapis::desktop;
    Workspace workspace(WorkspaceMode::preview);
    QTemporaryDir config;
    CHECK(config.isValid());
    KeyMap keymap;
    keymap.setSourcePathForTesting(config.filePath(QStringLiteral("strip.json")));
    AgentSearch search(&workspace);
    const auto usage = fake_usage(config);
    UiPreview preview(workspace, {.source = QUrl::fromLocalFile(QStringLiteral(LAPIS_QML_SOURCE)),
                                  .compact = false,
                                  .screen = QString(),
                                  .keymap = &keymap,
                                  .agentSearch = &search,
                                  .usage = usage.get()});
    CHECK(preview.load());
    auto* window = preview.window();
    window->resize(1400, 960);
    wait_active(*window);
    pump(60);
    const auto item = [window](const QString& name) {
        auto* found = find_visual(window->contentItem(), name);
        if (found == nullptr)
            throw std::runtime_error("missing item " + name.toStdString());
        return found;
    };
    const auto press = [&](const char* action) {
        send_binding(*window, keymap.sequences(QString::fromLatin1(action)).front());
        pump(260); // Longer than the strip's scroll animation.
    };
    const auto capture = [window](const char* name) {
        if (const auto path = qEnvironmentVariable("LAPIS_WORKSPACE_CAPTURE_PREFIX");
            !path.isEmpty())
            CHECK(window->grabWindow().save(path + QString::fromLatin1(name) +
                                            QStringLiteral(".png")));
    };
    auto* strip = item(QStringLiteral("agentTabs"));
    auto* stage = item(QStringLiteral("focusedPane"));
    auto* terminal = qobject_cast<TerminalSurface*>(item(QStringLiteral("liveTerminal")));
    CHECK(terminal != nullptr);
    CHECK(strip->isVisible() && strip->property("count").toInt() == 6);
    CHECK(strip->mapToScene(QPointF()).y() >= stage->mapToScene(QPointF()).y() + stage->height());
    CHECK(stage->mapToScene(QPointF()).y() <= 12); // No tab row above the stage.
    check_category_alignment(*window);
    auto* card_surface =
        qobject_cast<TerminalSurface*>(item(QStringLiteral("previewTerminal_agent")));
    CHECK(card_surface != nullptr && card_surface->document() == workspace.session("agent"));
    CHECK(!card_surface->interactive() && !card_surface->isEnabled() &&
          card_surface->frameInterval() == 250 && qFuzzyCompare(card_surface->minimumScale(), 0.5));
    click_visual(*window, *item(QStringLiteral("agentTab_agent")));
    pump(30);
    CHECK(focused_id(workspace) == QStringLiteral("agent") && terminal->hasActiveFocus());
    CHECK(!card_surface->hasActiveFocus());
    click_visual(*window, *item(QStringLiteral("agentTab_shell")));
    pump(30);
    CHECK(focused_id(workspace) == QStringLiteral("shell"));

    // Six cards overflow the strip. Stepping either way keeps the selected card
    // whole and part of the next one visible, as sidescrolloff does.
    const auto card_width = item(QStringLiteral("agentTab_shell"))->width();
    const auto check_scrolloff = [&](bool forward) {
        const auto list = workspace.categorySessions();
        const auto* card = item(QStringLiteral("agentTab_") + focused_id(workspace));
        const auto left = card->mapToItem(strip, QPointF()).x();
        const auto right = left + card->width();
        CHECK(left >= -1 && right <= strip->width() + 1);
        const bool first = list.front().value<SessionPreview*>() == workspace.focusedSession();
        if (forward)
            CHECK(strip->width() - right >= card_width * 0.35);
        else
            CHECK(first || left >= card_width * 0.35);
    };
    for (int step = 0; step < 5; ++step) {
        press("nextWindow");
        check_scrolloff(true);
    }
    capture("strip-end");
    for (int step = 0; step < 5; ++step) {
        press("previousWindow");
        check_scrolloff(false);
    }
    CHECK(focused_id(workspace) == QStringLiteral("shell"));
    capture("strip");

    // A finished turn pulses an unselected card; the selected agent never does.
    CHECK(workspace.selectSession(QStringLiteral("renderer")));
    pump(260);
    finish_turn(*workspace.session(QStringLiteral("agent")));
    finish_turn(*workspace.session(QStringLiteral("renderer")));
    pump(30);
    CHECK(workspace.session(QStringLiteral("agent"))->unseen());
    CHECK(!workspace.session(QStringLiteral("renderer"))->unseen());
    auto* cue = item(QStringLiteral("unseenCue_agent"));
    CHECK(cue->isVisible());
    CHECK(QQmlProperty::read(cue, QStringLiteral("border.color")).value<QColor>() ==
          window->property("textColor").value<QColor>());
    CHECK(!item(QStringLiteral("unseenCue_renderer"))->isVisible());
    // A pending request pulses in the attention color.
    {
        namespace wire = lapis::session::wire;
        wire::AttentionSnapshot request;
        request.available = request.connected = request.ready = true;
        request.activity = lapis::session::attention::Activity::turn_completed;
        request.requests.emplace_back();
        workspace.session(QStringLiteral("agent"))->applyAttention(request);
    }
    pump(30);
    CHECK(
        QQmlProperty::read(item(QStringLiteral("unseenCue_agent")), QStringLiteral("border.color"))
            .value<QColor>() == window->property("attentionColor").value<QColor>());
    capture("unseen");
    const QPointer<QQuickItem> replaced_cue = item(QStringLiteral("unseenCue_agent"));
    CHECK(replaced_cue != nullptr);
    // Category changes reset the strip model. Drain deferred deletes without
    // adding a sleep so the subsequent lookups prove they see fresh delegates.
    CHECK(workspace.addCategory(QStringLiteral("Other")));
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCoreApplication::processEvents();
    CHECK(replaced_cue.isNull());
    // Unseen agents elsewhere mark their category.
    const auto other = workspace.activeCategoryId();
    CHECK(workspace.selectCategory(QStringLiteral("general")));
    CHECK(workspace.moveSession(QStringLiteral("notes"), other));
    CHECK(workspace.selectSession(QStringLiteral("renderer")));
    pump(30);
    finish_turn(*workspace.session(QStringLiteral("notes")));
    pump(30);
    CHECK(item(QStringLiteral("categoryUnseen_") + other)->isVisible());
    CHECK(!item(QStringLiteral("categoryUnseen_general"))->isVisible());
    // Selecting the agent is looking at it.
    click_visual(*window, *item(QStringLiteral("agentTab_agent")));
    pump(30);
    CHECK(!workspace.session(QStringLiteral("agent"))->unseen() &&
          !item(QStringLiteral("unseenCue_agent"))->isVisible());
    CHECK(workspace.selectSession(QStringLiteral("notes")));
    pump(30);
    CHECK(!item(QStringLiteral("categoryUnseen_") + other)->isVisible());
    CHECK(workspace.selectCategory(QStringLiteral("general")));
    pump(30);
    // Assistive activation of a card selects its agent, as a click does.
    {
        const auto target = focused_id(workspace) == QStringLiteral("renderer")
                                ? QStringLiteral("agent")
                                : QStringLiteral("renderer");
        reveal_card(*window, workspace, target);
        auto* face =
            QAccessible::queryAccessibleInterface(item(QStringLiteral("agentTab_") + target));
        CHECK(face != nullptr && face->actionInterface() != nullptr);
        face->actionInterface()->doAction(QAccessibleActionInterface::pressAction());
        pump(30);
        CHECK(focused_id(workspace) == target);
    }

    // Short windows keep the strip with shorter cards.
    window->resize(640, 480);
    pump(80);
    CHECK(strip->isVisible() && strip->height() <= 100);
    check_stage_bounds(*window, *terminal);
    window->resize(1400, 960);
    pump(80);
    // Hiding previews keeps keyboard navigation.
    CHECK(keymap.setPreviewsVisible(false));
    pump(30);
    CHECK(!strip->isVisible());
    const auto before = focused_id(workspace);
    press("nextWindow");
    CHECK(focused_id(workspace) != before);
    CHECK(keymap.setPreviewsVisible(true));
    pump(30);
    CHECK(strip->isVisible());

    // Command-W on an agent without a process closes it; its right neighbor
    // takes the stage.
    CHECK(workspace.selectSession(QStringLiteral("agent")));
    pump(30);
    const auto list = workspace.categorySessions();
    QString after;
    for (qsizetype i = 0; i + 1 < list.size(); ++i)
        if (list[i].value<SessionPreview*>()->sessionId() == QStringLiteral("agent"))
            after = list[i + 1].value<SessionPreview*>()->sessionId();
    press("closeAgent");
    CHECK(workspace.session(QStringLiteral("agent")) == nullptr);
    CHECK(focused_id(workspace) == after && terminal->hasActiveFocus());

    // The new-agent card ends the strip.
    CHECK(QMetaObject::invokeMethod(strip, "positionViewAtEnd"));
    pump(30);
    click_visual(*window, *item(QStringLiteral("newAgentButton")));
    auto* picker = window->findChild<QObject*>(QStringLiteral("agentDialog"));
    CHECK(picker != nullptr);
    wait_popup(*picker, true);
    CHECK(QMetaObject::invokeMethod(picker, "close"));
    wait_popup(*picker, false);

    // The + under the last category opens the category form and shows its key.
    auto* new_category = item(QStringLiteral("newCategoryButton"));
    auto* last_category =
        item(QStringLiteral("category_") +
             workspace.categories().constLast().toMap().value(QStringLiteral("id")).toString());
    CHECK(new_category->mapToScene({0, 0}).y() >
          last_category->mapToScene({0, last_category->height()}).y() - 1);
#ifdef Q_OS_MACOS
    const QString category_key = QString(QChar(0x2318)) + QLatin1Char('N');
#else
    const QString category_key = QStringLiteral("Ctrl+Shift+N");
#endif
    CHECK(item(QStringLiteral("newCategoryHint"))->property("text").toString() == category_key);
    click_visual(*window, *new_category);
    auto* category_form = window->findChild<QObject*>(QStringLiteral("categoryDialog"));
    CHECK(category_form != nullptr);
    wait_popup(*category_form, true);
    CHECK(QMetaObject::invokeMethod(category_form, "close"));
    wait_popup(*category_form, false);

    check_agent_search(*window, workspace, keymap, *terminal);
    check_usage(*window, *usage, keymap, *terminal);
    CHECK(preview.diagnostics().isEmpty());
    return EXIT_SUCCESS;
}
} // namespace

int run_diagnostics_reentrancy_test() {
    QTemporaryDir directory;
    CHECK(directory.isValid());
    const auto path = write_qml(directory, QStringLiteral("warning.qml"), QStringLiteral(R"(
import QtQuick
Window {
    id: warningWindow
    visible: false
    width: 200; height: 200
    function warn() { throw new Error("runtime fixture warning") }
    Connections {
        target: preview
        function onDiagnosticsChanged() { throw new Error("diagnostics fixture warning") }
    }
    Timer { interval: 1; running: true; onTriggered: warningWindow.warn() }
}
)"));
    lapis::desktop::Workspace workspace(lapis::desktop::WorkspaceMode::preview);
    lapis::desktop::UiPreview preview(
        workspace, {.source = QUrl::fromLocalFile(path), .compact = true, .screen = QString()});
    int notifications = 0;
    QObject observation;
    QObject::connect(&preview, &lapis::desktop::UiPreview::diagnosticsChanged, &observation,
                     [&] { ++notifications; });
    CHECK(preview.load());
    QElapsedTimer deadline;
    deadline.start();
    while (preview.diagnostics().isEmpty() && deadline.elapsed() < 2000)
        pump(5);
    CHECK(preview.diagnostics().contains(QStringLiteral("runtime fixture warning")));
    CHECK(notifications == 1);
    CHECK(preview.diagnostics().size() <= 4096);
    return EXIT_SUCCESS;
}

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
    // Render-thread shutdown posts signal proxies to the GUI thread. Finish
    // their delivery and deferred deletion before destroying the application.
    const auto drain = qScopeGuard([] {
        QCoreApplication::sendPostedEvents();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    });
    try {
        if (app.arguments().contains(QStringLiteral("--shortcuts-only")))
            return run_shortcut_focus_tests();
        if (run_workspace_tests() != EXIT_SUCCESS || run_ui_tests() != EXIT_SUCCESS ||
            run_diagnostics_reentrancy_test() != EXIT_SUCCESS ||
            run_surface_tests() != EXIT_SUCCESS || run_attention_dialog_tests() != EXIT_SUCCESS ||
            run_attention_ui_tests() != EXIT_SUCCESS || run_strip_ui_tests() != EXIT_SUCCESS)
            return EXIT_FAILURE;
        std::cout << "ui_preview_test: PASS\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "ui_preview_test: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
