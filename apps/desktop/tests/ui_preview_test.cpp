#include "app_window.hpp"
#include "config.hpp"
#include "conversation_index.hpp"
#include "gui_state.hpp"
#include "model_change_recorder.hpp"
#include "next_prompt.hpp"
#include "platform/window_activation.hpp"
#include "terminal_surface.hpp"
#include "terminals.hpp"
#include "transcript.hpp"
#include "workspace.hpp"
#include <QAccessible>
#include <QElapsedTimer>
#include <QHash>
#include <QJSValue>
#include <QQmlEngine>
#include <QQmlProperty>
#include <QQuickStyle>
#include <QSet>
#include <QStandardPaths>
#include <QThread>

#include <QClipboard>
#include <QColor>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
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
#include <cmath>
#include <cstdlib>
#include <functional>
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
    lapis::desktop::Workspace workspace(lapis::desktop::WorkspaceMode::fixture);
    CHECK(workspace.fixtureMode());
    CHECK(workspace.focusedIndex() == 0);
    CHECK(workspace.focusedAgent() != nullptr);
    const int agent_count = static_cast<int>(workspace.agents().size());
    workspace.nextAgent(-1);
    CHECK(workspace.focusedIndex() == agent_count - 1);
    workspace.nextAgent();
    CHECK(workspace.focusedIndex() == 0);
    workspace.nextAgent(agent_count + 2);
    CHECK(workspace.focusedIndex() == 2);
    workspace.nextAgent(-2);
    CHECK(workspace.focusedIndex() == 0);

    const std::array<QString, 6> expected_ids = {
        QStringLiteral("shell"),   QStringLiteral("renderer"), QStringLiteral("agent"),
        QStringLiteral("service"), QStringLiteral("checks"),   QStringLiteral("notes")};
    const QVariantList agents = workspace.agents();
    CHECK(agents.size() == static_cast<qsizetype>(expected_ids.size()));
    std::size_t agent_index = 0;
    for (const auto& value : agents) {
        auto* agent = qvariant_cast<lapis::desktop::Agent*>(value);
        CHECK(agent != nullptr);
        CHECK(agent->agentId() == expected_ids.at(agent_index));
        CHECK(!agent->live());
        CHECK(!agent->attentionPending());
        CHECK(agent->attentionCount() == 0);
        CHECK(agent->attentionSerial() == 0);
        ++agent_index;
    }
    CHECK(workspace.agent(QStringLiteral("shell"))->agentId() == QStringLiteral("shell"));
    CHECK(workspace.agent(QStringLiteral("does-not-exist")) == nullptr);

    auto* agent = workspace.agent(QStringLiteral("agent"));
    AttentionRecorder recorder;
    QObject::connect(agent, &lapis::desktop::Agent::attentionChanged, &recorder,
                     [&recorder] { ++recorder.changed_count; });
    QObject::connect(agent, &lapis::desktop::Agent::attentionArrived, &recorder,
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

    auto* renderer = workspace.agent(QStringLiteral("renderer"));
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

    auto* checks = workspace.agent(QStringLiteral("checks"));
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
    lapis::desktop::Workspace workspace(lapis::desktop::WorkspaceMode::fixture);
    QTemporaryDir directory;
    CHECK(directory.isValid());
    const QString qml_path =
        write_qml(directory, QStringLiteral("valid.qml"),
                  QStringLiteral("import QtQuick\nWindow { id: root; objectName: \"preview-root\"; "
                                 "width: 481; height: 313; visible: false\nfunction reloadNow() { "
                                 "app.reload(); return 7; }\n}"));

    lapis::desktop::AppWindow preview(
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
        lapis::desktop::AppWindow placed(
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

    lapis::desktop::AppWindow unmatched(workspace, {.source = QUrl::fromLocalFile(qml_path),
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
        QObject::connect(&preview, &lapis::desktop::AppWindow::reducedMotionChanged,
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
                             "app.reload(); return 7; }\n}"));
    QPointer<QQuickWindow> old_window(window);
    CHECK(QMetaObject::invokeMethod(window, "reloadNow"));
    CHECK(preview.window() != nullptr);
    CHECK(preview.window() != old_window.data());
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    CHECK(!old_window);
    CHECK(preview.window()->objectName() == QStringLiteral("preview-root"));
    CHECK(preview.window()->geometry() == preserved_geometry);
    write_qml(directory, QStringLiteral("valid.qml"),
              QStringLiteral("import QtQuick\nWindow { visible: false; "
                             "property int invalidBinding: missingValue; "
                             "function reloadTwice() { app.reload(); app.reload(); return 7; } }"));
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

bool wait_for(const std::function<bool()>& ready, int milliseconds) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (!ready() && elapsed.elapsed() < milliseconds)
        pump(5);
    return ready();
}

void wait_terminal_focus(QQuickWindow& window, lapis::desktop::TerminalSurface& terminal) {
    lapis::desktop::test::activate_test_window(window);
    const auto ready = [&] {
        terminal.forceActiveFocus();
        return window.isActive() && window.isExposed() && terminal.hasActiveFocus() &&
               !terminal.composing() && !terminal.pasting();
    };
    CHECK(wait_for(ready, 5000));
    pump(10); // Let the queued focus-ownership work start after activation.
}

void wait_input_ready(QQuickWindow& window, lapis::desktop::TerminalSurface& terminal) {
    wait_terminal_focus(window, terminal);
    const auto ready = [&] {
        return !window.property("inputBlocked").toBool() &&
               window.property("shortcutsArmed").toBool() && terminal.hasActiveFocus();
    };
    CHECK(wait_for(ready, 5000));
}

void wait_sidebar(QQuickWindow& window, lapis::desktop::Config& config_model, bool visible) {
    CHECK(wait_for([&] { return config_model.sidebarVisible() == visible; }, 5000));
    CHECK(find_visual(window.contentItem(), QStringLiteral("categoryRail"))->isVisible() ==
          visible);
}

// Opens the Settings section that holds `item`, as a click on it would.
void show_setting_section(QQuickWindow& window, QQuickItem* item) {
    for (auto* row = item; row != nullptr; row = row->parentItem()) {
        if (!row->property("isSettingRow").toBool())
            continue;
        auto* dialog = window.findChild<QObject*>(QStringLiteral("settingsDialog"));
        CHECK(dialog != nullptr);
        CHECK(QMetaObject::invokeMethod(dialog, "showSection",
                                        Q_ARG(QVariant, row->property("section"))));
        pump(20);
        return;
    }
}

void click_setting(QQuickWindow& window, const QString& name) {
    auto* item = find_visual(window.contentItem(), name);
    auto* scroll = find_visual(window.contentItem(), QStringLiteral("settingsScroll"));
    CHECK(item != nullptr && scroll != nullptr);
    show_setting_section(window, item);
    CHECK(item->isVisible());
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
    // Appearance buttons are not checkable; callers verify the actual Config selection.
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
    Workspace workspace(WorkspaceMode::fixture);
    AppWindow preview(workspace, {.source = QUrl::fromLocalFile(QStringLiteral(LAPIS_QML_SOURCE)),
                                  .compact = true,
                                  .screen = QString()});
    CHECK(preview.load());
    auto* window = preview.window();
    wait_active(*window);
    auto* terminal = qobject_cast<TerminalSurface*>(
        find_visual(window->contentItem(), QStringLiteral("liveTerminal")));
    auto* dialog = window->findChild<QObject*>(QStringLiteral("attentionDialog"));
    CHECK(terminal != nullptr && dialog != nullptr);
    wait_terminal_focus(*window, *terminal);
    auto* document = workspace.focusedAgent();
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
    wait_for([&] { return !dialog->property("visible").toBool(); }, 5000);
    CHECK(!dialog->property("visible").toBool());
    wait_terminal_focus(*window, *terminal);
    CHECK(terminal->hasActiveFocus());
    CHECK(QMetaObject::invokeMethod(window, "openAttentionDialog"));
    wait_popup(*dialog, true);
    CHECK(window->property("inputBlocked").toBool());
    CHECK(!preview.assignTerminalFocus());
    CHECK(window->activeFocusItem() != terminal);
    CHECK(QMetaObject::invokeMethod(dialog, "selectRequest",
                                    Q_ARG(QVariant, document->attentionRequests()[0])));
    pump(20);
    // The question reads as text with nothing to type into: answers go to the
    // agent's own terminal.
    auto* question = find_visual(window->contentItem(), QStringLiteral("question-color"));
    auto* notice = find_visual(window->contentItem(), QStringLiteral("requestNotice"));
    CHECK(question != nullptr && notice != nullptr);
    CHECK(question->property("text").toString() == QStringLiteral("Choose a color"));
    CHECK(notice->property("text").toString() ==
          QStringLiteral("Answer in the originating terminal."));
    CHECK(find_visual(window->contentItem(), QStringLiteral("answer-color")) == nullptr);
    // Another request leaves the selection where it was.
    state.diagnostic = QStringLiteral("Another request arrived");
    auto other = state.requests[0];
    other.pending.request.id = std::string("other");
    other.pending.revision = 2;
    state.requests.push_back(other);
    document->applyAttention(state);
    pump(20);
    CHECK(question == find_visual(window->contentItem(), QStringLiteral("question-color")));
    CHECK(dialog->property("current").value<QVariantMap>().value(QStringLiteral("token")) ==
          document->attentionRequests()[0].toMap().value(QStringLiteral("token")));
    QKeyEvent navigate(QEvent::KeyPress, Qt::Key_Tab, Qt::ControlModifier);
    QCoreApplication::sendEvent(window, &navigate);
    CHECK(workspace.focusedIndex() == 0);
    CHECK(!preview.openSettings());
    CHECK(!window->findChild<QObject*>(QStringLiteral("settingsDialog"))
               ->property("visible")
               .toBool());
    // A retired request stays on screen, marked as no longer current.
    state.requests.erase(state.requests.begin());
    document->applyAttention(state);
    pump(20);
    CHECK(question->property("text").toString() == QStringLiteral("Choose a color"));
    CHECK(notice->property("text").toString() ==
          QStringLiteral("This request is no longer current."));
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
void check_terminal_font_controls(QQuickWindow& window, lapis::desktop::Config& config_model,
                                  lapis::desktop::TerminalSurface& terminal,
                                  const QStringList& families, const QTemporaryDir& directory) {
    using lapis::desktop::kTerminalFontSizeDefault;
    CHECK(config_model.terminalFontSize() == kTerminalFontSizeDefault);
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
    CHECK(config_model.terminalFontSize() == kTerminalFontSizeDefault + 1);
    CHECK(terminal.fontPixelSize() == kTerminalFontSizeDefault + 1);
    CHECK(terminal.gridSize().width() < base_grid.width());
    CHECK(grid_changes == 1);
    click_setting(window, QStringLiteral("fontSmaller"));
    CHECK(config_model.terminalFontSize() == kTerminalFontSizeDefault);
    CHECK(terminal.gridSize() == base_grid);

    auto* fontSizeField = find_visual(window.contentItem(), QStringLiteral("fontSizeValue"));
    CHECK(fontSizeField != nullptr);
    const auto replaceFontSize = [&](const QString& value) {
        fontSizeField->forceActiveFocus();
        CHECK(fontSizeField->hasActiveFocus());
        CHECK(QMetaObject::invokeMethod(fontSizeField, "selectAll"));
        for (const auto character : value) {
            QKeyEvent press(QEvent::KeyPress, 0, Qt::NoModifier, QString(character));
            QKeyEvent release(QEvent::KeyRelease, 0, Qt::NoModifier, QString(character));
            QCoreApplication::sendEvent(&window, &press);
            QCoreApplication::sendEvent(&window, &release);
        }
        pump(30);
    };
    const auto returnInFontSize = [&] {
        QKeyEvent press(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier, QStringLiteral("\r"));
        QKeyEvent release(QEvent::KeyRelease, Qt::Key_Return, Qt::NoModifier);
        QCoreApplication::sendEvent(&window, &press);
        QCoreApplication::sendEvent(&window, &release);
        pump(30);
    };

    // The validator rejects text that cannot be a number without touching size.
    replaceFontSize(QStringLiteral("wide"));
    CHECK(fontSizeField->property("text").toString() == QString::number(kTerminalFontSizeDefault));
    CHECK(config_model.terminalFontSize() == kTerminalFontSizeDefault);
    CHECK(terminal.fontPixelSize() == kTerminalFontSizeDefault);

    // An intermediate value is accepted while typing, but an out-of-range
    // commit restores the current saved value rather than corrupting it.
    replaceFontSize(QStringLiteral("99"));
    CHECK(fontSizeField->property("text").toString() == QStringLiteral("99"));
    returnInFontSize();
    CHECK(fontSizeField->property("text").toString() == QString::number(kTerminalFontSizeDefault));
    CHECK(config_model.terminalFontSize() == kTerminalFontSizeDefault);
    CHECK(terminal.fontPixelSize() == kTerminalFontSizeDefault);

    // Direct entry uses the same live-and-persisted path as the buttons.
    const int enteredSize = kTerminalFontSizeDefault + 6;
    replaceFontSize(QString::number(enteredSize));
    returnInFontSize();
    CHECK(fontSizeField->property("text").toString() == QString::number(enteredSize));
    CHECK(config_model.terminalFontSize() == enteredSize);
    CHECK(terminal.fontPixelSize() == enteredSize);
    lapis::desktop::Config directEntryPersisted;
    directEntryPersisted.setSourcePathForTesting(directory.filePath(QStringLiteral("lapis.json")));
    CHECK(directEntryPersisted.load());
    CHECK(directEntryPersisted.terminalFontSize() == enteredSize);

    // The display follows the config_model after an external +/- adjustment.
    click_setting(window, QStringLiteral("fontSmaller"));
    CHECK(config_model.terminalFontSize() == enteredSize - 1);
    CHECK(terminal.fontPixelSize() == enteredSize - 1);
    CHECK(fontSizeField->property("text").toString() == QString::number(enteredSize - 1));
    click_setting(window, QStringLiteral("fontLarger"));
    CHECK(config_model.terminalFontSize() == enteredSize);
    CHECK(fontSizeField->property("text").toString() == QString::number(enteredSize));

    // Empty input on focus loss restores the current size. A valid size on
    // focus loss commits through the same path without needing Return.
    fontSizeField->forceActiveFocus();
    CHECK(QMetaObject::invokeMethod(fontSizeField, "selectAll"));
    QKeyEvent erase(QEvent::KeyPress, Qt::Key_Backspace, Qt::NoModifier);
    QCoreApplication::sendEvent(&window, &erase);
    QKeyEvent eraseRelease(QEvent::KeyRelease, Qt::Key_Backspace, Qt::NoModifier);
    QCoreApplication::sendEvent(&window, &eraseRelease);
    CHECK(fontSizeField->property("text").toString().isEmpty());
    terminal.forceActiveFocus();
    pump(30);
    CHECK(fontSizeField->property("text").toString() == QString::number(enteredSize));
    replaceFontSize(QString::number(kTerminalFontSizeDefault));
    terminal.forceActiveFocus();
    pump(30);
    CHECK(config_model.terminalFontSize() == kTerminalFontSizeDefault);
    for (int attempt = 0; attempt < 100 && terminal.gridSize() != base_grid; ++attempt)
        pump(10);
    CHECK(terminal.gridSize() == base_grid);

    // A missing family falls back to the system fixed-width face and says so.
    CHECK(config_model.setTerminalFontFamily(QStringLiteral("lapis missing mono")));
    pump(30);
    CHECK(terminal.resolvedFontFamily() == system_family);
    auto* unavailable = find_visual(window.contentItem(), QStringLiteral("fontUnavailable"));
    CHECK(unavailable != nullptr && unavailable->isVisible());
    if (!families.isEmpty()) {
        CHECK(config_model.setTerminalFontFamily(families.front()));
        pump(30);
        CHECK(terminal.resolvedFontFamily() == families.front());
        CHECK(window.property("monoFamily").toString() == families.front());
        CHECK(!unavailable->isVisible());
    }
    CHECK(config_model.setTerminalFontFamily(QString()));
    pump(30);
    CHECK(terminal.resolvedFontFamily() == system_family);

    const QString config = directory.filePath(QStringLiteral("lapis.json"));
    const QString saved = read_text(config);
    write_config(directory, QStringLiteral("{unfinished"));
    for (int attempt = 0; attempt < 100 && !config_model.diagnostic().contains(
                                               QStringLiteral("not a valid JSON object"));
         ++attempt)
        pump(10);
    CHECK(config_model.diagnostic().contains(QStringLiteral("not a valid JSON object")));
    pump(100); // Let duplicate file-watcher deliveries settle before counting grids.
    const QSize failed_save_grid = terminal.gridSize();
    const int changes_before_failure = grid_changes;
    click_setting(window, QStringLiteral("fontLarger"));
    CHECK(grid_changes == changes_before_failure); // No tentative resize on a failed save.
    CHECK(config_model.terminalFontSize() == kTerminalFontSizeDefault);
    CHECK(terminal.fontPixelSize() == kTerminalFontSizeDefault);
    CHECK(terminal.gridSize() == failed_save_grid);
    CHECK(config_model.diagnostic().contains(QStringLiteral("Could not save")));
    write_config(directory, saved);
    CHECK(config_model.reload());
    CHECK(config_model.diagnostic().isEmpty());
}

// Closed Commands and Settings expose empty models and do not recompute for
// workspace changes. Opening either resolves the entries for the state the
// user would see, including a category with no focused agent.
void check_lazy_command_models(QQuickWindow& window, lapis::desktop::Workspace& workspace,
                               lapis::desktop::AppWindow& preview) {
    auto* commands = window.findChild<QObject*>(QStringLiteral("commandsDialog"));
    auto* settings = window.findChild<QObject*>(QStringLiteral("settingsDialog"));
    CHECK(commands != nullptr && settings != nullptr);

    ModelChangeRecorder command_observer;
    ModelChangeRecorder shortcut_observer;
    const auto slot_for = [](QObject& receiver) {
        const auto* meta = receiver.metaObject();
        const int slot_index = meta->indexOfSlot("changed()");
        CHECK(slot_index >= 0);
        return meta->method(slot_index);
    };
    const auto observe = [](QObject& dialog, const char* name, QObject& receiver,
                            const QMetaMethod& slot) {
        const auto* meta = dialog.metaObject();
        const int property_index = meta->indexOfProperty(name);
        CHECK(property_index >= 0);
        const QMetaProperty property = meta->property(property_index);
        CHECK(property.isValid() && property.hasNotifySignal());
        CHECK(QObject::connect(&dialog, property.notifySignal(), &receiver, slot));
    };
    observe(*commands, "commands", command_observer, slot_for(command_observer));
    observe(*settings, "shortcutRows", shortcut_observer, slot_for(shortcut_observer));
    const auto model = [](QObject& dialog, const char* name) {
        return dialog.property(name).toList();
    };
    const auto entry = [](const QVariantList& entries, const QString& id) {
        for (const auto& value : entries)
            if (value.toMap().value(QStringLiteral("id")).toString() == id)
                return value.toMap();
        return QVariantMap{};
    };
    const auto open_commands = [&] {
        CHECK(QMetaObject::invokeMethod(&window, "openCommandsDialog"));
        auto* dialog = window.findChild<QObject*>(QStringLiteral("commandsDialog"));
        CHECK(dialog != nullptr);
        wait_popup(*dialog, true);
        return model(*dialog, "commands");
    };

    CHECK(model(*commands, "commands").isEmpty());
    CHECK(model(*settings, "shortcutRows").isEmpty());
    auto* initial = workspace.focusedAgent();
    CHECK(initial != nullptr);
    const QString initial_id = initial->agentId();
    const QString initial_category = workspace.activeCategoryId();

    CHECK(workspace.addCategory(QStringLiteral("Lazy commands")));
    pump(30);
    CHECK(workspace.focusedAgent() == nullptr);
    CHECK(model(*commands, "commands").isEmpty());
    CHECK(model(*settings, "shortcutRows").isEmpty());
    CHECK(command_observer.changes == 0 && shortcut_observer.changes == 0);

    const auto empty_category_commands = open_commands();
    CHECK(!empty_category_commands.isEmpty());
    CHECK(!entry(empty_category_commands, QStringLiteral("closeAgent"))
               .value(QStringLiteral("enabled"))
               .toBool());
    CHECK(QMetaObject::invokeMethod(commands, "close"));
    wait_popup(*commands, false);
    command_observer.changes = 0;
    shortcut_observer.changes = 0;

    CHECK(workspace.selectCategory(initial_category));
    pump(30);
    CHECK(workspace.focusedAgent() != nullptr);
    const auto agents = workspace.categoryAgents();
    CHECK(agents.size() >= 2);
    QString alternate;
    for (const auto& value : agents) {
        auto* agent = value.value<lapis::desktop::Agent*>();
        if (agent != nullptr && agent->agentId() != initial_id) {
            alternate = agent->agentId();
            break;
        }
    }
    CHECK(!alternate.isEmpty());

    CHECK(workspace.selectAgent(alternate));
    pump(30);
    CHECK(model(*commands, "commands").isEmpty());
    CHECK(model(*settings, "shortcutRows").isEmpty());
    CHECK(command_observer.changes == 0);
    CHECK(shortcut_observer.changes == 0);
    CHECK(workspace.selectAgent(initial_id));
    pump(30);
    CHECK(model(*commands, "commands").isEmpty());
    CHECK(model(*settings, "shortcutRows").isEmpty());
    CHECK(command_observer.changes == 0 && shortcut_observer.changes == 0);

    const auto agent_commands = open_commands();
    CHECK(entry(agent_commands, QStringLiteral("closeAgent"))
              .value(QStringLiteral("enabled"))
              .toBool());
    CHECK(QMetaObject::invokeMethod(commands, "close"));
    wait_popup(*commands, false);

    CHECK(preview.openSettings());
    wait_popup(*settings, true);
    const auto shortcut_rows = model(*settings, "shortcutRows");
    CHECK(!shortcut_rows.isEmpty());
#ifdef Q_OS_MACOS
    const QString new_agent_keys = QStringLiteral("⌘T");
#else
    const QString new_agent_keys = QStringLiteral("Ctrl+Shift+T");
#endif
    bool found_new_agent = false;
    for (const auto& value : shortcut_rows) {
        const auto row = value.toMap();
        found_new_agent |= row.value(QStringLiteral("keys")).toString() == new_agent_keys;
    }
    CHECK(found_new_agent);
    CHECK(QMetaObject::invokeMethod(settings, "close"));
    wait_popup(*settings, false);
    CHECK(model(*commands, "commands").isEmpty());
    CHECK(model(*settings, "shortcutRows").isEmpty());
}

int run_shortcut_focus_tests() {
    using namespace lapis::desktop;
    Workspace workspace(WorkspaceMode::fixture);
    QTemporaryDir directory;
    CHECK(directory.isValid());
    write_config(directory, QStringLiteral(R"({"version":1,"layout":"focus","theme":"lapis",)"
                                           R"("keybindings":{"openSettings":["Ctrl+Alt+T"]}})"));
    Config config_model;
    config_model.setSourcePathForTesting(directory.filePath(QStringLiteral("lapis.json")));
    CHECK(config_model.load());

    AppWindow preview(workspace, {.source = QUrl::fromLocalFile(QStringLiteral(LAPIS_QML_SOURCE)),
                                  .compact = true,
                                  .screen = QString(),
                                  .config = &config_model});
    CHECK(preview.load());
    auto* window = preview.window();
    window->show();
    window->requestActivate();
    wait_active(*window);
    auto* terminal = find_visual(window->contentItem(), QStringLiteral("liveTerminal"));
    auto* dialog = window->findChild<QObject*>(QStringLiteral("settingsDialog"));
    CHECK(terminal != nullptr && dialog != nullptr);
    terminal->forceActiveFocus();
    check_lazy_command_models(*window, workspace, preview);

    CHECK(QMetaObject::invokeMethod(dialog, "open"));
    wait_popup(*dialog, true);
    const int focused_at_modal = workspace.focusedIndex();
    const QString theme_at_modal = config_model.themeName();
    const QString config_at_modal = read_text(directory.filePath(QStringLiteral("lapis.json")));
    int changes_during_modal = 0;
    const auto changed_connection = QObject::connect(
        &config_model, &Config::changed, [&changes_during_modal] { ++changes_during_modal; });
    for (const auto* action :
         {"nextWindow", "previousWindow", "nextCategory", "previousCategory", "category1",
          "category2", "newAgent", "newCategory", "reloadConfig"}) {
        for (const auto& binding : config_model.sequences(QString::fromLatin1(action))) {
            const auto combination = QKeySequence(binding)[0];
            QKeyEvent press(QEvent::KeyPress, combination.key(), combination.keyboardModifiers());
            QKeyEvent release(QEvent::KeyRelease, combination.key(),
                              combination.keyboardModifiers());
            QCoreApplication::sendEvent(window, &press);
            QCoreApplication::sendEvent(window, &release);
            pump(10);
            CHECK(workspace.focusedIndex() == focused_at_modal);
            CHECK(config_model.themeName() == theme_at_modal);
            CHECK(changes_during_modal == 0);
        }
    }
    QObject::disconnect(changed_connection);
    CHECK(workspace.focusedIndex() == focused_at_modal);
    CHECK(config_model.themeName() == theme_at_modal);
    CHECK(read_text(directory.filePath(QStringLiteral("lapis.json"))) == config_at_modal);
    CHECK(dialog->property("opened").toBool());
    CHECK(window->activeFocusItem() != terminal);
    CHECK(QMetaObject::invokeMethod(dialog, "close"));
    wait_popup(*dialog, false);
    CHECK(workspace.focusedIndex() == focused_at_modal);
    CHECK(config_model.themeName() == theme_at_modal);

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

    CHECK(config_model.setTheme(QStringLiteral("graphite")));
    pump(50);
    CHECK(dialog->property("opened").toBool());
    CHECK(window->activeFocusItem() != terminal);

    CHECK(config_model.setDensity(QStringLiteral("compact")));
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
        CHECK(config_model.themeName() == QString::fromLatin1(theme));
        CHECK(dialog->property("opened").toBool());
    }
    for (const auto& density : {"comfortable", "compact", "minimal"}) {
        click_setting(*window, QStringLiteral("choice-") + QString::fromLatin1(density));
        CHECK(config_model.densityName() == QString::fromLatin1(density));
    }
    click_setting(*window, QStringLiteral("theme-lapis"));
    check_terminal_font_controls(*window, config_model, *qobject_cast<TerminalSurface*>(terminal),
                                 preview.monospaceFamilies(), directory);
    Config persisted;
    persisted.setSourcePathForTesting(directory.filePath(QStringLiteral("lapis.json")));
    CHECK(persisted.load());
    CHECK(persisted.themeName() == config_model.themeName());
    CHECK(persisted.densityName() == config_model.densityName());
    CHECK(persisted.terminalFontSize() == config_model.terminalFontSize());
    CHECK(persisted.terminalFontFamily() == config_model.terminalFontFamily());
    CHECK(QMetaObject::invokeMethod(dialog, "close"));
    wait_popup(*dialog, false);

    write_config(directory, QStringLiteral(R"({"keybindings":{"nextCategory":["Ctrl+Alt+Y"]}})"));
    CHECK(config_model.reload());
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
void check_cursor_rendering(lapis::desktop::Agent& document, QQuickWindow& window) {
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
int run_surface_tests(bool background) {
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

    Workspace workspace(WorkspaceMode::fixture);
    QQuickWindow window;
    window.resize(420, 220);
    auto* surface = new TerminalSurface(window.contentItem()); // parent owns it
    surface->setSize(window.size());
    window.show();
    pump(50); // An empty surface also crosses the render boundary.
    auto* document = workspace.focusedAgent();
    surface->setDocument(document);
    pump(50);
    const auto original = document->snapshot();
    // Pixel geometry is qualified on the production GPU backend. The software
    // rasterizer differs at stroked edges; logical lifecycle checks below run in both.
    if (!background)
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

// The preview throttle gates publish-to-publish: after a quiet gap a change
// reaches the card in the same frame-coalesced render as the stage, inside
// the gate the card holds its last frame, and when the gate reopens the card
// jumps to the newest snapshot, never a queued one.
int run_preview_frame_sync_tests() {
    using namespace lapis::desktop;
    using namespace lapis::session;
    Agent document{QStringLiteral("frame-sync"), QStringLiteral("/tmp"), {}, QColor(Qt::white), ""};
    QQuickWindow window;
    window.resize(440, 220);
    auto* stage = new TerminalSurface(window.contentItem()); // parent owns it
    auto* card = new TerminalSurface(window.contentItem());
    stage->setObjectName(QStringLiteral("syncStage"));
    card->setObjectName(QStringLiteral("syncCard"));
    stage->setSize(QSizeF(200, 200));
    card->setX(240);
    card->setSize(QSizeF(200, 200));
    card->setFrameInterval(250);
    window.show();
    pump(60);
    stage->setDocument(&document);
    card->setDocument(&document);
    pump(60);

    TerminalSnapshot snapshot;
    snapshot.size = {4, 2};
    snapshot.cells.resize(8);
    snapshot.cursor = {.column = 0, .row = 0, .in_viewport = true, .visible = false};
    const QRgb first = qRgb(0x10, 0x20, 0x30);
    const QRgb second = qRgb(0x40, 0x50, 0x60);
    const QRgb third = qRgb(0x70, 0x80, 0x90);
    const auto paint = [&](QRgb color) {
        snapshot.background_rgb = static_cast<std::uint32_t>(color & 0xffffffU);
        ++snapshot.revision;
        document.applySnapshot(snapshot);
    };
    const auto grab_region = [&](const TerminalSurface* surface) {
        const QImage image = window.grabWindow();
        CHECK(!image.isNull());
        // grabWindow() returns device pixels; crop the surface's mapped bounds.
        const qreal dpr = image.devicePixelRatio() > 0 ? image.devicePixelRatio() : 1.0;
        const QRectF scene = surface->mapRectToScene(QRectF(QPointF{}, surface->size()));
        return image.copy(QRect(qRound(scene.left() * dpr), qRound(scene.top() * dpr),
                                qRound(scene.width() * dpr), qRound(scene.height() * dpr)));
    };
    const auto area = [](const QImage& region, QRgb color) {
        return colored_area(region, color).pixels;
    };

    paint(first);
    pump(500); // The initial paint passes the card's gate; let it settle.
    CHECK(area(grab_region(stage), first) > 0);
    CHECK(area(grab_region(card), first) > 0);

    // A keystroke after a quiet gap: both surfaces present the same frame.
    paint(second);
    pump(20);
    CHECK(area(grab_region(stage), second) > 0);
    CHECK(area(grab_region(card), second) > 0);

    // Output inside the gate advances the stage while the card holds.
    paint(third);
    pump(20);
    CHECK(area(grab_region(stage), third) > 0);
    CHECK(area(grab_region(card), second) > 0);
    CHECK(area(grab_region(card), third) == 0);

    // Once the gate reopens the card catches up to the latest snapshot.
    pump(500);
    CHECK(area(grab_region(card), third) > 0);
    CHECK(area(grab_region(card), second) == 0);
    return EXIT_SUCCESS;
}

// Stage surfaces only: preview cards are scaled copies, not terminals, and
// the side terminal is a shell beside the stage.
bool preview_surface(const QQuickItem* item) {
    return item->objectName().startsWith(QStringLiteral("previewTerminal_")) ||
           item->objectName() == QStringLiteral("sideTerminalSurface");
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

void check_accessible_terminal(QQuickItem& terminal, const QString& name) {
    auto* face = QAccessible::queryAccessibleInterface(&terminal);
    CHECK(face != nullptr && face->role() == QAccessible::EditableText);
    CHECK(face->text(QAccessible::Name) == name);
    CHECK(face->state().focusable);
    CHECK(face->state().focused && terminal.hasActiveFocus());
    // Never advertise an editable target after the terminal stops accepting
    // input; dictation sends to the focused accessible element.
    CHECK(face->state().editable == terminal.property("interactive").toBool());
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
void click_with(QQuickWindow& window, QQuickItem& item, Qt::KeyboardModifiers modifiers) {
    const auto position = item.mapToScene(QPointF(item.width() / 2, item.height() / 2));
    const auto global = window.mapToGlobal(position);
    QMouseEvent press(QEvent::MouseButtonPress, position, global, Qt::LeftButton, Qt::LeftButton,
                      modifiers);
    QMouseEvent release(QEvent::MouseButtonRelease, position, global, Qt::LeftButton, Qt::NoButton,
                        modifiers);
    QCoreApplication::sendEvent(&window, &press);
    QCoreApplication::sendEvent(&window, &release);
}
// Press on `item`, move to `to` in steps as a hand would, and release there.
void drag_visual(QQuickWindow& window, QQuickItem& item, QPointF to) {
    const auto from = item.mapToScene(QPointF(item.width() / 2, item.height() / 2));
    // Qt drops pointer moves that repeat a timestamp, so each event gets its own.
    static quint64 stamp = 1'000'000;
    const auto send = [&](QEvent::Type type, QPointF at, Qt::MouseButtons buttons) {
        QMouseEvent event(type, at, window.mapToGlobal(at),
                          type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton, buttons,
                          Qt::NoModifier);
        event.setTimestamp(stamp += 16);
        QCoreApplication::sendEvent(&window, &event);
        pump(8);
    };
    send(QEvent::MouseButtonPress, from, Qt::LeftButton);
    constexpr int kSteps = 12;
    for (int step = 1; step <= kSteps; ++step)
        send(QEvent::MouseMove, from + (to - from) * step / kSteps, Qt::LeftButton);
    send(QEvent::MouseButtonRelease, to, Qt::NoButton);
    pump(80);
}
QRectF scene_rect(const QQuickItem& item) {
    return {item.mapToScene(QPointF()), QSizeF(item.width(), item.height())};
}
QStringList strip_ids(const lapis::desktop::Workspace& workspace) {
    QStringList ids;
    for (const auto& value : workspace.categoryAgents())
        ids.append(value.value<lapis::desktop::Agent*>()->agentId());
    return ids;
}
// Every agent in the sidebar's order: each category's agents in turn.
QStringList sidebar_ids(const lapis::desktop::Workspace& workspace) {
    QStringList ids;
    for (const auto& group : workspace.categories())
        for (const auto& value : group.toMap().value(QStringLiteral("agents")).toList())
            ids.append(value.value<lapis::desktop::Agent*>()->agentId());
    return ids;
}

// The stage and strip drags, through the window as a person would use them.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void check_stage_and_drags(QQuickWindow& window, lapis::desktop::Workspace& workspace,
                           lapis::desktop::Config& config_model) {
    const auto item = [&window](const QString& name) {
        auto* found = find_visual(window.contentItem(), name);
        if (found == nullptr)
            throw std::runtime_error("missing item " + name.toStdString());
        return found;
    };
    const auto key = [&](const char* action) {
        send_binding(window, config_model.sequences(QString::fromLatin1(action)).front());
        pump(80);
    };
    auto* stage = item(QStringLiteral("focusedPane"));
    auto* terminal =
        qobject_cast<lapis::desktop::TerminalSurface*>(item(QStringLiteral("liveTerminal")));
    auto* visual = item(QStringLiteral("visualView"));
    const auto ids = strip_ids(workspace);
    const auto strip_category = workspace.activeCategoryId();
    CHECK(terminal != nullptr && ids.size() >= 3);
    CHECK(workspace.selectAgent(ids[0]));
    pump(40);

    // The stage shows one agent, the selected one, filling it; the visual view
    // lies exactly over its terminal.
    const auto fills_stage = [&](const QString& id) {
        CHECK(workspace.focusedAgent() == workspace.agent(id));
        CHECK(terminal->document() == workspace.agent(id));
        const auto shown = scene_rect(*terminal);
        const auto edge = scene_rect(*stage);
        CHECK(edge.contains(shown) && shown.width() > edge.width() - 20 &&
              shown.height() > edge.height() - 20);
        CHECK(scene_rect(*visual) == shown);
    };
    fills_stage(ids[0]);
    // Shown, the visual view covers exactly that terminal. The sample agents
    // have no saved conversation, so it is shown directly, then hidden again.
    visual->setVisible(true);
    pump(40);
    CHECK(scene_rect(*visual) == scene_rect(*terminal));
    if (const auto path = qEnvironmentVariable("LAPIS_WORKSPACE_CAPTURE_PREFIX"); !path.isEmpty())
        CHECK(window.grabWindow().save(path + QStringLiteral("stage-visual.png")));
    visual->setVisible(false);
    pump(20);

    // A card dropped on the stage does not split it.
    const auto strip_before = strip_ids(workspace);
    drag_visual(window, *item(QStringLiteral("agentTab_") + ids[1]),
                stage->mapToScene(QPointF(stage->width() * 0.92, stage->height() * 0.5)));
    pump(40);
    CHECK(strip_ids(workspace) == strip_before);
    CHECK(terminal->document() == workspace.focusedAgent());
    CHECK(workspace.selectAgent(ids[0]));
    pump(40);

    // The next and previous agent keys walk the sidebar: through this
    // category, on into the next one's agents, and around; the stage
    // terminal follows every press.
    const auto walk = sidebar_ids(workspace);
    CHECK(walk.size() > ids.size() && walk.indexOf(ids[0]) == 0);
    for (qsizetype step = 1; step <= walk.size(); ++step) {
        key("nextWindow");
        fills_stage(walk[step % walk.size()]);
    }
    key("previousWindow");
    fills_stage(walk.constLast());
    CHECK(workspace.activeCategoryId() != strip_category);
    key("nextWindow");
    fills_stage(ids[0]);
    CHECK(workspace.activeCategoryId() == strip_category);

    // Clicking a card shows it on the stage.
    click_visual(window, *item(QStringLiteral("agentTab_") + ids[2]));
    pump(40);
    fills_stage(ids[2]);

    // Dragging a card along the strip reorders it.
    const auto order = strip_ids(workspace);
    auto* last_card = item(QStringLiteral("agentTab_") + order.constLast());
    // The last card's far edge, or the strip's own where the card runs past it.
    const auto strip_right = scene_rect(*item(QStringLiteral("agentTabs"))).right() - 4;
    auto drop_at = last_card->mapToScene(QPointF(last_card->width() - 4, last_card->height() / 2));
    drop_at.setX(qMin(drop_at.x(), strip_right));
    drag_visual(window, *item(QStringLiteral("agentTab_") + order[0]), drop_at);
    CHECK(strip_ids(workspace).constLast() == order[0]);
    CHECK(workspace.placeAgents({order[0]}, workspace.activeCategoryId(), 0));
    CHECK(strip_ids(workspace) == order);

    // Command-click picks a second card; dragging either carries both to a
    // category in the rail.
#ifdef Q_OS_MACOS
    const auto toggle = Qt::MetaModifier;
#else
    const auto toggle = Qt::ControlModifier;
#endif
    const auto home = workspace.activeCategoryId();
    const auto elsewhere = workspace.createCategory(QStringLiteral("Drop target"));
    CHECK(!elsewhere.isEmpty());
    pump(60);
    CHECK(workspace.selectAgent(ids[1]));
    pump(40);
    click_with(window, *item(QStringLiteral("agentTab_") + ids[2]), toggle);
    pump(40);
    CHECK(item(QStringLiteral("pickedCue_") + ids[2])->isVisible());
    auto* target = item(QStringLiteral("category_") + elsewhere);
    drag_visual(window, *item(QStringLiteral("agentTab_") + ids[2]),
                target->mapToScene(QPointF(target->width() / 2, target->height() / 2)));
    const auto moved = strip_ids(workspace);
    CHECK(!moved.contains(ids[1]) && !moved.contains(ids[2]));
    CHECK(workspace.agentPlace(ids[1]).value(QStringLiteral("category")).toString() ==
          QStringLiteral("Drop target"));
    // Back where they were, so later checks see the original strip.
    CHECK(workspace.placeAgents({ids[1], ids[2]}, home, 1));
    CHECK(workspace.selectCategory(home));
    CHECK(workspace.removeCategory(elsewhere));
    pump(60);
    CHECK(strip_ids(workspace) == order);
}
// Command-plus and zero change the text size; categories reorder.
void check_text_size(QQuickWindow& window, lapis::desktop::Workspace& workspace,
                     lapis::desktop::Config& config_model) {
    const auto item = [&window](const QString& name) {
        auto* found = find_visual(window.contentItem(), name);
        if (found == nullptr)
            throw std::runtime_error("missing item " + name.toStdString());
        return found;
    };
    const auto key = [&](const char* action) {
        send_binding(window, config_model.sequences(QString::fromLatin1(action)).front());
        pump(80);
    };
    auto* terminal =
        qobject_cast<lapis::desktop::TerminalSurface*>(item(QStringLiteral("liveTerminal")));
    for (const auto& value : workspace.categoryAgents()) {
        auto* agent = value.value<lapis::desktop::Agent*>();
        if (agent->title() == QStringLiteral("Codex"))
            CHECK(workspace.selectAgent(agent->agentId()));
    }
    pump(60);

    const int size = config_model.terminalFontSize();
    key("textBigger");
    CHECK(config_model.terminalFontSize() == size + 1 && terminal->fontPixelSize() == size + 1);
    key("textSmaller");
    CHECK(config_model.terminalFontSize() == size);
    key("textReset");
    CHECK(config_model.terminalFontSize() == config_model.terminalFontSizeDefault());

    const auto second = workspace.createCategory(QStringLiteral("Moved up"));
    CHECK(workspace.placeCategory(second, 0));
    CHECK(workspace.categories().constFirst().toMap().value(QStringLiteral("id")).toString() ==
          second);
    CHECK(workspace.removeCategory(second));
    pump(40);
}
void check_composition_navigation(lapis::desktop::Workspace& workspace,
                                  lapis::desktop::AppWindow& preview,
                                  lapis::desktop::TerminalSurface& terminal) {
    using namespace lapis::desktop;
    QTemporaryDir directory;
    CHECK(directory.isValid());
    auto* document = workspace.focusedAgent();
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
    Config defaults;
    for (const auto* action :
         {"nextWindow", "previousWindow", "nextCategory", "previousCategory", "category1",
          "category2", "newAgent", "newCategory", "openCommands", "toggleSidebar"})
        for (const auto& binding : defaults.sequences(QString::fromLatin1(action))) {
            send_binding(*preview.window(), binding);
            CHECK(workspace.focusedAgent() == document);
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
        CHECK(wait_for(
            [&] {
                return workspace.focusedAgent() == document &&
                       workspace.activeCategoryId() == category && terminal.composing() &&
                       terminal.hasActiveFocus() &&
                       !preview.window()->property("inputBlocked").toBool();
            },
            5000));
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
    CHECK(workspace.focusedAgent() == document);
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
            CHECK(workspace.focusedAgent() == document);
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
    const auto list = workspace.categoryAgents();
    for (qsizetype i = 0; i < list.size(); ++i)
        if (list[i].value<lapis::desktop::Agent*>()->agentId() == id)
            CHECK(QMetaObject::invokeMethod(strip, "positionViewAtIndex", Q_ARG(int, int(i)),
                                            Q_ARG(int, 2 /* ListView.Contain */)));
    pump(30);
}

// Keyboard focus, activity and lost connections stay visually distinct, and each
// status class has a non-color shape cue on its tab: a filled dot at work, a
// hollow ring once done, a diamond in the attention colour waiting on an answer.
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
        shapes.value(QStringLiteral("working")), shapes.value(QStringLiteral("idle")),
        shapes.value(QStringLiteral("waiting")), shapes.value(QStringLiteral("disconnected")),
        shapes.value(QStringLiteral("ended")),   shapes.value(QStringLiteral("unknown"))};
    CHECK(classes.size() == 6);
    CHECK(shapes.value(QStringLiteral("working")) == QStringLiteral("dot"));
    CHECK(shapes.value(QStringLiteral("finished")) == QStringLiteral("ring"));
    CHECK(shapes.value(QStringLiteral("idle")) == shapes.value(QStringLiteral("finished")));
    CHECK(shapes.value(QStringLiteral("waiting")) == QStringLiteral("diamond"));
    CHECK(shapes.value(QStringLiteral("connecting")) == shapes.value(QStringLiteral("unknown")));
    const auto focus = window.property("focusedBorderColor").value<QColor>();
    const auto attention = window.property("attentionColor").value<QColor>();
    CHECK(focus.isValid() && attention.isValid());
    CHECK(colors.value(QStringLiteral("working")) != focus);
    CHECK(colors.value(QStringLiteral("working")) != colors.value(QStringLiteral("finished")));
    CHECK(colors.value(QStringLiteral("waiting")) == attention);
    for (const auto& kind : {QStringLiteral("disconnected"), QStringLiteral("ended")}) {
        CHECK(colors.value(kind) != attention);
        CHECK(colors.value(kind) != focus);
    }
    namespace wire = lapis::session::wire;
    wire::AttentionSnapshot working;
    working.available = working.connected = working.ready = true;
    working.activity = lapis::session::attention::Activity::working;
    workspace.agent(QStringLiteral("checks"))->applyAttention(working);
    auto idle = working;
    idle.activity = lapis::session::attention::Activity::idle;
    workspace.agent(QStringLiteral("notes"))->applyAttention(idle);
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
              shapes.value(workspace.agent(id)->statusKind()));
    }
    // The sample agent waits on an answer: its own mark, not a finished turn's.
    CHECK(workspace.agent(QStringLiteral("agent"))->statusKind() == QStringLiteral("waiting"));
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
        return tab.width() > 0 && view.width() > 0 && tab.isVisible() && view.isVisible() &&
               position.x() >= -1 && position.x() + tab.width() <= view.width() + 1;
    };
    QElapsedTimer deadline;
    deadline.start();
    QMetaObject::invokeMethod(&view, "revealFocused");
    while (!visible() && deadline.elapsed() < 5000) {
        pump(5);
    }
    CHECK(visible());
}

int run_attention_ui_tests() {
    using namespace lapis::desktop;
    Workspace workspace(WorkspaceMode::fixture);
    QTemporaryDir palette_config;
    CHECK(palette_config.isValid());
    Config config_model;
    config_model.setSourcePathForTesting(palette_config.filePath(QStringLiteral("palette.json")));
    // The checks below follow the selected card along the opt-in strip.
    CHECK(config_model.setPreviewsVisible(true));
    AppWindow preview(workspace, {.source = QUrl::fromLocalFile(QStringLiteral(LAPIS_QML_SOURCE)),
                                  .compact = false,
                                  .screen = QString(),
                                  .config = &config_model});
    CHECK(preview.load());
    auto* window = preview.window();
    wait_active(*window);
    pump(50);
    auto* terminal = qobject_cast<TerminalSurface*>(
        find_visual(window->contentItem(), QStringLiteral("liveTerminal")));
    CHECK(terminal != nullptr);
    terminal->forceActiveFocus();
    auto* original = workspace.focusedAgent();
    const auto stage_size = terminal->size();
    CHECK(workspace.replayAttention(QStringLiteral("two")));
    pump(50);
    check_status_semantics(*window, workspace);
    CHECK(workspace.focusedAgent() == original);
    CHECK(terminal->document() == original);
    CHECK(terminal->hasActiveFocus());
    CHECK(terminal->size() == stage_size);
    CHECK(workspace.categories().front().toMap().value(QStringLiteral("attentionCount")).toInt() ==
          2);
    CHECK(!workspace.replayAttention(QStringLiteral("duplicate")));
    CHECK(workspace.replayAttention(QStringLiteral("resolve")));
    CHECK(workspace.agent(QStringLiteral("renderer"))->attentionPending());
    CHECK(workspace.focusedAgent() == original);
    preview.setReducedMotion(true);
    CHECK(workspace.replayAttention(QStringLiteral("arrival")));
    CHECK(workspace.focusedAgent() == original);

    CHECK(workspace.selectAgent(QStringLiteral("agent")));
    auto* selected = workspace.focusedAgent();
    CHECK(workspace.addCategory(QStringLiteral("Research")));
    const auto research = workspace.activeCategoryId();
    pump(20);
    CHECK(workspace.focusedAgent() == nullptr);
    CHECK(terminal->document() == nullptr);
    CHECK(workspace.moveAgent(QStringLiteral("renderer"), research));
    pump(20);
    CHECK(terminal->document() == workspace.agent(QStringLiteral("renderer")));
    workspace.nextAgent();
    CHECK(workspace.focusedAgent() == workspace.agent(QStringLiteral("renderer")));
    CHECK(workspace.selectCategory(QStringLiteral("general")));
    pump(20);
    CHECK(workspace.focusedAgent() == selected);
    CHECK(terminal->document() == selected);
    CHECK(workspace.moveAgentBy(selected->agentId(), 1));
    pump(20);
    CHECK(terminal->document() == selected);
    CHECK(workspace.agent(selected->agentId()) == selected);
    auto* category_button =
        find_visual(window->contentItem(), QStringLiteral("category_") + research);
    CHECK(category_button != nullptr);
    click_visual(*window, *category_button);
    pump(20);
    CHECK(workspace.activeCategoryId() == research);
    CHECK(terminal->document() == workspace.agent(QStringLiteral("renderer")));
    category_button = find_visual(window->contentItem(), QStringLiteral("category_general"));
    CHECK(category_button != nullptr);
    click_visual(*window, *category_button);
    pump(20);
    CHECK(workspace.focusedAgent() == selected);
    auto* tab = find_visual(window->contentItem(), QStringLiteral("agentTab_shell"));
    CHECK(tab != nullptr);
    const auto original_tab_width = tab->width();
    click_visual(*window, *tab);
    pump(20);
    CHECK(workspace.focusedAgent() == original);
    CHECK(tab->width() == original_tab_width);
    tab = find_visual(window->contentItem(), QStringLiteral("agentTab_agent"));
    CHECK(tab != nullptr);
    click_visual(*window, *tab);
    pump(20);
    CHECK(workspace.focusedAgent() == selected);
    CHECK(workspace.renameAgent(selected->agentId(), QString(80, QLatin1Char('W'))));
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
            CHECK(config_model.setDensity(QString::fromLatin1(variant.density)));
            CHECK(config_model.setTerminalFontSize(variant.font_size));
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
                                             QStringLiteral("agentTab_") + selected->agentId());
            CHECK(tabs != nullptr && selected_tab != nullptr);
            wait_tab_visible(*selected_tab, *tabs);
            check_category_alignment(*window);
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
    CHECK(config_model.setDensity(QStringLiteral("comfortable")));
    CHECK(config_model.setTerminalFontSize(lapis::desktop::kTerminalFontSizeDefault));
    if (const auto path = qEnvironmentVariable("LAPIS_WORKSPACE_CAPTURE_PREFIX"); !path.isEmpty()) {
        // Visual review only: semantic colors across contrasting themes, and
        // Appearance with its font controls.
        window->resize(1400, 960);
        for (const auto* theme : {"daylight", "contrast", "amber", "oled", "lapis"}) {
            CHECK(config_model.setTheme(QString::fromLatin1(theme)));
            pump(80);
            CHECK(window->grabWindow().save(path + QStringLiteral("theme-") +
                                            QString::fromLatin1(theme) + QStringLiteral(".png")));
        }
        CHECK(preview.openSettings());
        auto* settings = window->findChild<QObject*>(QStringLiteral("settingsDialog"));
        wait_popup(*settings, true);
        CHECK(window->grabWindow().save(path + QStringLiteral("appearance.png")));
        // The visual view's themes, fonts and size, scrolled into view.
        auto* scroll = find_visual(window->contentItem(), QStringLiteral("settingsScroll"));
        auto* themes = find_visual(window->contentItem(), QStringLiteral("visualThemes"));
        CHECK(scroll != nullptr && themes != nullptr);
        scroll->setProperty("contentY", themes->mapToItem(scroll, QPointF()).y() - 60);
        pump(40);
        CHECK(window->grabWindow().save(path + QStringLiteral("appearance-visual.png")));
        scroll->setProperty("contentY", 0);
        CHECK(QMetaObject::invokeMethod(settings, "close"));
        wait_popup(*settings, false);
    }
    // Category/sidebar navigation does not replace the selected agent.
    window->resize(1400, 960);
    wait_input_ready(*window, *terminal);
    send_binding(*window, config_model.sequences(QStringLiteral("toggleSidebar")).front());
    wait_sidebar(*window, config_model, false);
    CHECK(terminal->document() == selected);
    send_binding(*window, config_model.sequences(QStringLiteral("toggleSidebar")).front());
    wait_sidebar(*window, config_model, true);
    auto* stage = find_visual(window->contentItem(), QStringLiteral("focusedPane"));
    CHECK(stage != nullptr);
    const auto stage_edge = [stage] {
        return QQmlProperty::read(stage, QStringLiteral("border.color")).value<QColor>();
    };
    CHECK(stage_edge() == window->property("focusedBorderColor").value<QColor>());
    send_binding(*window, config_model.sequences(QStringLiteral("openCommands")).front());
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
    return workspace.focusedAgent() ? workspace.focusedAgent()->agentId() : QString();
}
void finish_turn(lapis::desktop::Agent& agent) {
    namespace wire = lapis::session::wire;
    wire::AttentionSnapshot state;
    state.available = state.connected = state.ready = true;
    state.activity = lapis::session::attention::Activity::working;
    agent.applyAttention(state);
    state.activity = lapis::session::attention::Activity::turn_completed;
    agent.applyAttention(state);
}

QQuickItem* required_visual(QQuickWindow& window, const QString& name) {
    auto* found = find_visual(window.contentItem(), name);
    if (found == nullptr)
        throw std::runtime_error("missing item " + name.toStdString());
    return found;
}
void press_action(QQuickWindow& window, const lapis::desktop::Config& config_model,
                  const char* action) {
    send_binding(window, config_model.sequences(QString::fromLatin1(action)).front());
    pump(260); // Longer than the strip's scroll animation.
}
void capture_step(QQuickWindow& window, const char* name) {
    if (const auto path = qEnvironmentVariable("LAPIS_WORKSPACE_CAPTURE_PREFIX"); !path.isEmpty())
        CHECK(window.grabWindow().save(path + QString::fromLatin1(name) + QStringLiteral(".png")));
}

// With nothing open, the stage lists what to do next and takes the keyboard:
// up and down move, Return runs the entry. Command-O finds a past conversation
// by typing and resumes it with Return.
void check_home_and_resume(QQuickWindow& window, lapis::desktop::Workspace& workspace,
                           const lapis::desktop::Config& config_model) {
    const auto before = workspace.activeCategoryId();
    CHECK(workspace.addCategory(QStringLiteral("Empty")));
    pump(60);
    auto* home = required_visual(window, QStringLiteral("homeList"));
    CHECK(home->isVisible() && home->hasActiveFocus());
    const auto rows = home->property("count").toInt();
    int recent = 0;
    for (int row = 0; row < rows; ++row)
        recent += find_visual(home, QStringLiteral("home_conversation_%1").arg(row)) != nullptr;
    CHECK(rows >= 5 && recent == 2);
    capture_step(window, "home");
    for (int step = 1; step < rows; ++step)
        send_binding(window, QStringLiteral("Down"));
    pump(30);
    CHECK(home->property("currentIndex").toInt() == rows - 1);
    // The last entry is the category the test came from; Return goes back.
    send_binding(window, QStringLiteral("Return"));
    pump(60);
    CHECK(workspace.activeCategoryId() == before);
    press_action(window, config_model, "resumeConversation");
    auto* resume = window.findChild<QObject*>(QStringLiteral("resumeDialog"));
    CHECK(resume != nullptr);
    wait_popup(*resume, true);
    auto* field = required_visual(window, QStringLiteral("resumeSearchField"));
    CHECK(field->hasActiveFocus());
    auto* results = required_visual(window, QStringLiteral("resumeResults"));
    CHECK(results->property("count").toInt() == 2);
    capture_step(window, "resume");
    field->setProperty("text", QStringLiteral("codex"));
    pump(30);
    CHECK(results->property("count").toInt() == 1);
    CHECK(required_visual(window,
                          QStringLiteral("resumeResult_0a1b2c3d-0000-4000-8000-000000000002")) !=
          nullptr);
    send_binding(window, QStringLiteral("Return"));
    wait_popup(*resume, false);
    pump(30);
    // The fixture starts no processes; the attempt reaches the workspace.
    CHECK(workspace.workspaceError().contains(QStringLiteral("preview")));
    workspace.clearError();
    for (const auto& value : workspace.categories())
        if (value.toMap().value(QStringLiteral("name")).toString() == QStringLiteral("Empty"))
            CHECK(workspace.removeCategory(value.toMap().value(QStringLiteral("id")).toString()));
    CHECK(workspace.selectCategory(before));
    pump(60);
}

// Command-1 to Command-9 (Control-Shift on Linux) pick the first nine
// categories.
void check_nine_categories(QQuickWindow& window, lapis::desktop::Workspace& workspace,
                           const lapis::desktop::Config& config_model) {
    const auto before = workspace.activeCategoryId();
    QStringList added;
    for (int number = 1; workspace.categories().size() < 9; ++number) {
        CHECK(workspace.addCategory(QStringLiteral("Numbered %1").arg(number)));
        added.append(workspace.activeCategoryId());
    }
    CHECK(workspace.selectCategory(before));
    pump(60);
    const auto id_at = [&workspace](int index) {
        return workspace.categories().at(index).toMap().value(QStringLiteral("id")).toString();
    };
    press_action(window, config_model, "category9");
    CHECK(workspace.activeCategoryId() == id_at(8));
    press_action(window, config_model, "category5");
    CHECK(workspace.activeCategoryId() == id_at(4));
    for (const auto& id : added)
        CHECK(workspace.removeCategory(id));
    CHECK(workspace.selectCategory(before));
    pump(60);
}

// The new-agent form offers this Mac and the ssh config's hosts: right and
// left change the machine while up and down choose the CLI, and another
// machine's folder starts at its home with no local suggestions.
void check_new_agent_machine(QQuickWindow& window, const lapis::desktop::Config& config_model) {
    press_action(window, config_model, "newAgent");
    auto* form = window.findChild<QObject*>(QStringLiteral("agentDialog"));
    CHECK(form != nullptr);
    wait_popup(*form, true);
    pump(60);
    CHECK(required_visual(window, QStringLiteral("machine_mac"))->isVisible() &&
          required_visual(window, QStringLiteral("machine_devbox"))->isVisible());
    CHECK(form->property("selectedMachine").toString().isEmpty());
    send_binding(window, QStringLiteral("Right"));
    pump(30);
    CHECK(form->property("selectedMachine").toString() == QStringLiteral("devbox"));
    capture_step(window, "new-agent-machine");
    send_binding(window, QStringLiteral("Return"));
    pump(60);
    CHECK(form->property("phase").toInt() == 1);
    CHECK(required_visual(window, QStringLiteral("agentDirectoryField"))
              ->property("text")
              .toString() == QStringLiteral("~"));
    CHECK(!required_visual(window, QStringLiteral("folderResults"))->isVisible() &&
          required_visual(window, QStringLiteral("agentMachine"))->isVisible());
    // Back to the CLI step and to this Mac, which the next form remembers.
    send_binding(window, QStringLiteral("Escape"));
    pump(30);
    send_binding(window, QStringLiteral("Left"));
    pump(30);
    CHECK(form->property("selectedMachine").toString().isEmpty());
    CHECK(QMetaObject::invokeMethod(form, "close"));
    wait_popup(*form, false);
}

QString screen_text(const lapis::session::TerminalSnapshot& snapshot) {
    QString text;
    for (std::size_t index = 0; index < snapshot.cells.size(); ++index) {
        const auto cell = snapshot.text(index);
        text += cell.empty() ? QStringLiteral(" ")
                             : QString::fromUcs4(cell.data(), static_cast<qsizetype>(cell.size()));
        if ((index + 1) % snapshot.size.columns == 0)
            text += QLatin1Char('\n');
    }
    return text;
}

void type_text(QQuickWindow& window, const QString& text) {
    for (const auto letter : text) {
        QKeyEvent press(QEvent::KeyPress, 0, Qt::NoModifier, QString(letter));
        QKeyEvent release(QEvent::KeyRelease, 0, Qt::NoModifier, QString(letter));
        QCoreApplication::sendEvent(&window, &press);
        QCoreApplication::sendEvent(&window, &release);
    }
}

bool pump_until(const std::function<bool()>& condition, int milliseconds) {
    QElapsedTimer clock;
    clock.start();
    while (!condition() && clock.elapsed() < milliseconds)
        pump(20);
    return condition();
}

// Command-` shows a plain shell beside the agents, with the keyboard; typing
// runs in it, and the same key hides it and gives the agent the keys back.
// Command-~ lists the machines by keyboard and Return opens that terminal.
void check_side_terminal(QQuickWindow& window, lapis::desktop::Terminals& terminals,
                         const lapis::desktop::Config& config_model,
                         lapis::desktop::TerminalSurface& stage) {
    press_action(window, config_model, "toggleTerminal");
    auto* panel = required_visual(window, QStringLiteral("sideTerminal"));
    CHECK(panel->isVisible());
    const bool ready = pump_until(
        [&terminals] {
            return terminals.current() != nullptr && terminals.current()->inputReady();
        },
        10000);
    if (!ready)
        qWarning().noquote() << "side terminal:" << terminals.error()
                             << (terminals.current() ? terminals.current()->connectionState()
                                                     : QStringLiteral("none"));
    CHECK(ready);
    pump(60);
    auto* surface = required_visual(window, QStringLiteral("sideTerminalSurface"));
    auto* side_input = qobject_cast<lapis::desktop::TerminalSurface*>(surface);
    CHECK(side_input != nullptr);
    CHECK(surface->hasActiveFocus());
    const auto original_interactive = surface->property("interactive").toBool();
    surface->setProperty("interactive", false);
    check_accessible_terminal(*surface, QStringLiteral("Side terminal"));
    auto* face = QAccessible::queryAccessibleInterface(surface);
    CHECK(face != nullptr && !face->state().editable);
    surface->setProperty("interactive", original_interactive);
    check_accessible_terminal(*surface, QStringLiteral("Side terminal"));
    type_text(window, QStringLiteral("hello"));
    send_binding(window, QStringLiteral("Return"));
    CHECK(pump_until(
        [&terminals] {
            return terminals.current() != nullptr && screen_text(terminals.current()->snapshot())
                                                         .contains(QStringLiteral("ran hello"));
        },
        10000));

    // Scrolling a side shell into history must not make its surface inert when
    // the panel is hidden and shown again: the first typed key returns live.
    type_text(window, QStringLiteral("history:2000"));
    send_binding(window, QStringLiteral("Return"));
    CHECK(pump_until(
        [&terminals] {
            return terminals.current() != nullptr && screen_text(terminals.current()->snapshot())
                                                         .contains(QStringLiteral("history done"));
        },
        10000));
    terminals.current()->olderHistory();
    CHECK(pump_until(
        [&terminals] {
            return terminals.current()->historyActive() &&
                   !terminals.current()->historyRequestPending();
        },
        10000));
    press_action(window, config_model, "toggleTerminal");
    CHECK(wait_for([&] { return !panel->isVisible() && stage.hasActiveFocus(); }, 5000));
    press_action(window, config_model, "toggleTerminal");
    CHECK(wait_for([&] { return panel->isVisible() && surface->hasActiveFocus(); }, 5000));
    CHECK(surface->property("interactive").toBool());
    type_text(window, QStringLiteral("after-history"));
    send_binding(window, QStringLiteral("Return"));
    CHECK(pump_until(
        [&terminals] {
            return terminals.current() != nullptr && !terminals.current()->historyActive() &&
                   screen_text(terminals.current()->snapshot())
                       .contains(QStringLiteral("ran after-history"));
        },
        10000));

    // Composition owns window shortcuts even in the side shell. In particular,
    // the key that hides this panel must not steal and discard the preedit.
    QInputMethodEvent composition(QStringLiteral("side-composition"), {});
    QCoreApplication::sendEvent(side_input, &composition);
    CHECK(side_input->composing() && side_input->hasActiveFocus());
    CHECK(window.property("terminalBusy").toBool() &&
          !window.property("interactionArmed").toBool() &&
          !window.property("shortcutsArmed").toBool());
    press_action(window, config_model, "toggleTerminal");
    pump(40);
    CHECK(panel->isVisible() && side_input->composing() && side_input->hasActiveFocus());
    QInputMethodEvent cancel;
    QCoreApplication::sendEvent(side_input, &cancel);
    pump(40);
    CHECK(!side_input->composing() && !window.property("terminalBusy").toBool() &&
          window.property("shortcutsArmed").toBool());
    capture_step(window, "side-terminal");
    // Command-W closes what is in front: the panel, not the agent behind it.
    const auto* agent = stage.document();
    const auto* shown = terminals.current();
    press_action(window, config_model, "closeAgent");
    CHECK(wait_for(
        [&] {
            return !panel->isVisible() && stage.hasActiveFocus() && agent != nullptr &&
                   stage.document() == agent;
        },
        5000));
    CHECK(terminals.current() == shown && shown->inputReady());
    press_action(window, config_model, "toggleTerminal");
    CHECK(wait_for([&] { return panel->isVisible() && surface->hasActiveFocus(); }, 5000));
    press_action(window, config_model, "toggleTerminal");
    CHECK(wait_for([&] { return !panel->isVisible() && stage.hasActiveFocus(); }, 5000));
    press_action(window, config_model, "chooseTerminal");
    auto* picker = window.findChild<QObject*>(QStringLiteral("terminalPicker"));
    CHECK(picker != nullptr);
    wait_popup(*picker, true);
    auto* machines = required_visual(window, QStringLiteral("terminalMachines"));
    CHECK(machines->hasActiveFocus() && machines->property("count").toInt() >= 1);
    CHECK(required_visual(window, QStringLiteral("terminalMachine_mac")) != nullptr);
    capture_step(window, "terminal-picker");
    send_binding(window, QStringLiteral("Return"));
    wait_popup(*picker, false);
    pump(60);
    CHECK(panel->isVisible() && surface->hasActiveFocus());
    press_action(window, config_model, "toggleTerminal");
    CHECK(!panel->isVisible());
    // The shell's service outlives lapis by design; end it here.
    const auto id = terminals.current()->agentId();
    CHECK(terminals.close(id));
    CHECK(pump_until([&terminals, &id] { return terminals.terminal(id) == nullptr; }, 10000));
}

// One saved conversation of each CLI, as they write them, and a stand-in
// shell that answers each line it reads.
void write_history_and_shell(const QTemporaryDir& config) {
    const auto save = [&config](const QString& relative, const QByteArray& text) {
        const auto path = config.filePath(relative);
        CHECK(QDir().mkpath(QFileInfo(path).absolutePath()));
        QFile file(path);
        CHECK(file.open(QIODevice::WriteOnly) && file.write(text) == text.size());
    };
    save(QStringLiteral(
             "history/claude/projects/-dev-lapis/0a1b2c3d-0000-4000-8000-000000000001.jsonl"),
         R"({"type":"system","entrypoint":"cli","cwd":"/dev/lapis"})"
         "\n"
         R"({"type":"user","message":{"role":"user","content":"Fix resize"}})"
         "\n");
    save(QStringLiteral("history/codex/sessions/2026/09/25/rollout-a.jsonl"),
         R"({"type":"session_meta","payload":{"id":"0a1b2c3d-0000-4000-8000-000000000002",)"
         R"("cwd":"/dev/api","source":"cli","thread_source":"user"}})"
         "\n");
    save(QStringLiteral("history/codex/session_index.jsonl"),
         R"({"id":"0a1b2c3d-0000-4000-8000-000000000002","thread_name":"Add retries"})"
         "\n");
    save(QStringLiteral("shell"), "#!/bin/sh\necho \"shell ready\"\nwhile read line; do\n"
                                  "  [ \"$line\" = exit ] && exit 0\n"
                                  "  case \"$line\" in\n"
                                  "    history:*)\n"
                                  "      echo \"ran $line\"\n"
                                  "      count=${line#history:}\n"
                                  "      index=0\n"
                                  "      while [ \"$index\" -lt \"$count\" ]; do\n"
                                  "        printf 'history %04d\\n' \"$index\"\n"
                                  "        index=$((index + 1))\n"
                                  "      done\n"
                                  "      echo \"history done\"\n"
                                  "      ;;\n"
                                  "    *) echo \"ran $line\" ;;\n"
                                  "  esac\ndone\n");
    CHECK(QFile::setPermissions(config.filePath(QStringLiteral("shell")),
                                QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    save(QStringLiteral("ssh_config"), "Host devbox\nHost *\n");
    CHECK(QDir().mkpath(config.filePath(QStringLiteral("runtime"))));
    CHECK(QFile::setPermissions(config.filePath(QStringLiteral("runtime")),
                                QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
}

void wait_ready(lapis::desktop::ConversationIndex& conversations) {
    conversations.refresh();
    QElapsedTimer scanned;
    scanned.start();
    while (!conversations.ready() && scanned.elapsed() < 10000)
        pump(20);
    CHECK(conversations.all().size() == 2);
}

// The agent strip is the category's navigation: live previews in tab order
// that never take input or resize a terminal, keep part of the neighboring
// card in view as selection moves, and pulse an agent that finished or needs a
// response until it is selected.
// The strip narrative is chronological and only one point above the threshold.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
int run_strip_ui_tests() {
    using namespace lapis::desktop;
    Workspace workspace(WorkspaceMode::fixture);
    // Leave room for terminal IDs inside the POSIX local-socket path limit.
    QTemporaryDir config(QStringLiteral("/tmp/lapis-ui-XXXXXX"));
    CHECK(config.isValid());
    Config config_model;
    config_model.setSourcePathForTesting(config.filePath(QStringLiteral("strip.json")));
    // The strip is opt-in; these checks are about it.
    CHECK(config_model.setPreviewsVisible(true));
    ConversationIndex conversations(config.filePath(QStringLiteral("history/claude")),
                                    config.filePath(QStringLiteral("history/codex")),
                                    config.filePath(QStringLiteral("conversations.json")));
    write_history_and_shell(config);
    wait_ready(conversations);
    Terminals terminals(config.filePath(QStringLiteral("runtime")),
                        config.filePath(QStringLiteral("ssh_config")));
    terminals.setShellForTesting(config.filePath(QStringLiteral("shell")));
    AppWindow preview(workspace, {.source = QUrl::fromLocalFile(QStringLiteral(LAPIS_QML_SOURCE)),
                                  .compact = false,
                                  .screen = QString(),
                                  .config = &config_model,
                                  .conversations = &conversations,
                                  .terminals = &terminals});
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
        send_binding(*window, config_model.sequences(QString::fromLatin1(action)).front());
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
    const auto press_ready = [&](const char* action) {
        wait_input_ready(*window, *terminal);
        send_binding(*window, config_model.sequences(QString::fromLatin1(action)).front());
        pump(260); // Longer than the strip's scroll animation.
    };
    CHECK(strip->isVisible() && strip->property("count").toInt() == 6);
    CHECK(strip->mapToScene(QPointF()).y() >= stage->mapToScene(QPointF()).y() + stage->height());
    CHECK(stage->mapToScene(QPointF()).y() <= 12); // No tab row above the stage.
    check_category_alignment(*window);
    auto* card_surface =
        qobject_cast<TerminalSurface*>(item(QStringLiteral("previewTerminal_agent")));
    CHECK(card_surface != nullptr && card_surface->document() == workspace.agent("agent"));
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
    const auto scrolloff_ready = [&](bool forward) {
        const auto list = workspace.categoryAgents();
        const auto* card = item(QStringLiteral("agentTab_") + focused_id(workspace));
        const auto left = card->mapToItem(strip, QPointF()).x();
        const auto right = left + card->width();
        if (!(left >= -1 && right <= strip->width() + 1))
            return false;
        const bool first = list.front().value<Agent*>() == workspace.focusedAgent();
        if (forward)
            return strip->width() - right >= card_width * 0.35;
        return first || left >= card_width * 0.35;
    };
    for (int step = 0; step < 5; ++step) {
        press_ready("nextWindow");
        CHECK(wait_for([&] { return scrolloff_ready(true); }, 5000));
    }
    capture("strip-end");
    for (int step = 0; step < 5; ++step) {
        press_ready("previousWindow");
        CHECK(wait_for([&] { return scrolloff_ready(false); }, 5000));
    }
    CHECK(wait_for([&] { return focused_id(workspace) == QStringLiteral("shell"); }, 5000));
    capture("strip");

    // A finished turn changes a card's status mark and nothing else: no cue
    // says whether it was looked at, and a card looks the same either way.
    CHECK(workspace.selectAgent(QStringLiteral("renderer")));
    pump(260);
    finish_turn(*workspace.agent(QStringLiteral("agent")));
    finish_turn(*workspace.agent(QStringLiteral("renderer")));
    pump(30);
    CHECK(workspace.agent(QStringLiteral("agent"))->unseen());
    CHECK(!workspace.agent(QStringLiteral("renderer"))->unseen());
    CHECK(find_visual(window->contentItem(), QStringLiteral("unseenCue_agent")) == nullptr);
    const auto card_mark = [&](const QString& id) {
        reveal_card(*window, workspace, id);
        return item(QStringLiteral("statusMark_") + id);
    };
    CHECK(card_mark(QStringLiteral("agent"))->property("shape").toString() ==
          QStringLiteral("ring"));
    CHECK(card_mark(QStringLiteral("agent"))->property("tone") ==
          card_mark(QStringLiteral("renderer"))->property("tone"));
    // A pending request shows its own mark, in the attention colour.
    {
        namespace wire = lapis::session::wire;
        wire::AttentionSnapshot request;
        request.available = request.connected = request.ready = true;
        request.activity = lapis::session::attention::Activity::turn_completed;
        request.requests.emplace_back();
        workspace.agent(QStringLiteral("agent"))->applyAttention(request);
    }
    pump(30);
    CHECK(card_mark(QStringLiteral("agent"))->property("shape").toString() ==
          QStringLiteral("diamond"));
    CHECK(card_mark(QStringLiteral("agent"))->property("tone").value<QColor>() ==
          window->property("attentionColor").value<QColor>());
    capture("unseen");
    // No category counts or marks agents nobody looked at.
    CHECK(workspace.addCategory(QStringLiteral("Other")));
    pump(30);
    const auto other = workspace.activeCategoryId();
    CHECK(workspace.selectCategory(QStringLiteral("general")));
    CHECK(workspace.moveAgent(QStringLiteral("notes"), other));
    CHECK(workspace.selectAgent(QStringLiteral("renderer")));
    pump(30);
    finish_turn(*workspace.agent(QStringLiteral("notes")));
    pump(30);
    CHECK(workspace.agent(QStringLiteral("notes"))->unseen());
    CHECK(find_visual(window->contentItem(), QStringLiteral("categoryUnseen_") + other) == nullptr);
    // Selecting the agent is looking at it.
    click_visual(*window, *item(QStringLiteral("agentTab_agent")));
    pump(30);
    CHECK(!workspace.agent(QStringLiteral("agent"))->unseen());
    CHECK(workspace.selectAgent(QStringLiteral("notes")));
    pump(30);
    CHECK(!workspace.agent(QStringLiteral("notes"))->unseen());
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
    // The stage terminal is a focused, editable text field to accessibility
    // clients: Wispr Flow pastes into a text field at once and falls back to
    // a slow paste when the focused element is the bare window.
    {
        wait_terminal_focus(*window, *terminal);
        auto* face = QAccessible::queryAccessibleInterface(terminal);
        CHECK(face != nullptr && face->role() == QAccessible::EditableText);
        CHECK(face->state().focusable && face->state().editable);
        CHECK(face->text(QAccessible::Name) == QStringLiteral("Terminal"));
        CHECK(face->state().editable == terminal->property("interactive").toBool());
        CHECK(terminal->hasActiveFocus() && face->state().focused);
    }

    // Short windows keep the strip with shorter cards.
    window->resize(640, 480);
    pump(80);
    CHECK(strip->isVisible() && strip->height() <= 100);
    check_stage_bounds(*window, *terminal);
    window->resize(1400, 960);
    pump(80);
    // Hiding previews keeps keyboard navigation.
    CHECK(config_model.setPreviewsVisible(false));
    pump(30);
    CHECK(!strip->isVisible());
    const auto before = focused_id(workspace);
    press_ready("nextWindow");
    CHECK(focused_id(workspace) != before);
    CHECK(config_model.setPreviewsVisible(true));
    pump(30);
    CHECK(strip->isVisible());

    // Command-W on an agent without a process closes it; its right neighbor
    // takes the stage.
    CHECK(workspace.selectAgent(QStringLiteral("agent")));
    pump(30);
    const auto list = workspace.categoryAgents();
    QString after;
    for (qsizetype i = 0; i + 1 < list.size(); ++i)
        if (list[i].value<Agent*>()->agentId() == QStringLiteral("agent"))
            after = list[i + 1].value<Agent*>()->agentId();
    press("closeAgent");
    CHECK(workspace.agent(QStringLiteral("agent")) == nullptr);
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

    check_stage_and_drags(*window, workspace, config_model);
    check_text_size(*window, workspace, config_model);
    CHECK(workspace.selectAgent(QStringLiteral("notes")));
    pump(30);
    check_home_and_resume(*window, workspace, config_model);
    check_nine_categories(*window, workspace, config_model);
    check_side_terminal(*window, terminals, config_model, *terminal);
    workspace.setSshConfigForTesting(config.filePath(QStringLiteral("ssh_config")));
    check_new_agent_machine(*window, config_model);
    CHECK(preview.diagnostics().isEmpty());
    return EXIT_SUCCESS;
}

void right_click(QQuickWindow& window, QQuickItem& item) {
    const auto position = item.mapToScene(QPointF(item.width() / 2, item.height() / 2));
    const auto global = window.mapToGlobal(position);
    QMouseEvent press(QEvent::MouseButtonPress, position, global, Qt::RightButton, Qt::RightButton,
                      Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease, position, global, Qt::RightButton, Qt::NoButton,
                        Qt::NoModifier);
    QCoreApplication::sendEvent(&window, &press);
    QCoreApplication::sendEvent(&window, &release);
}

// The sidebar is the navigation: each category a colour-coded group with its
// agents listed under it. A row shows its agent (and category), the agent on
// the stage is marked in its group's colour, a row's menu is the strip card's,
// rows drag onto categories, and the category menu sets the colour.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
int run_sidebar_ui_tests() {
    using namespace lapis::desktop;
    Workspace workspace(WorkspaceMode::fixture);
    QTemporaryDir config(QStringLiteral("/tmp/lapis-ui-XXXXXX"));
    CHECK(config.isValid());
    Config config_model;
    config_model.setSourcePathForTesting(config.filePath(QStringLiteral("sidebar.json")));
    // The sidebar navigates; the strip is off until asked for.
    CHECK(!config_model.previewsVisible());
    AppWindow preview(workspace, {.source = QUrl::fromLocalFile(QStringLiteral(LAPIS_QML_SOURCE)),
                                  .compact = false,
                                  .screen = QString(),
                                  .config = &config_model});
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
    const auto capture = [window](const char* name) {
        if (const auto path = qEnvironmentVariable("LAPIS_WORKSPACE_CAPTURE_PREFIX");
            !path.isEmpty())
            CHECK(window->grabWindow().save(path + QStringLiteral("sidebar-") +
                                            QString::fromLatin1(name) + QStringLiteral(".png")));
    };
    auto* terminal = qobject_cast<TerminalSurface*>(item(QStringLiteral("liveTerminal")));
    CHECK(terminal != nullptr);

    // General: Codex, Workspace notes. Build: Renderer, Agent bridge, Checks.
    // Research: Session service. Ideas: empty.
    const auto build = workspace.createCategory(QStringLiteral("Build"));
    const auto research = workspace.createCategory(QStringLiteral("Research"));
    const auto ideas = workspace.createCategory(QStringLiteral("Ideas"));
    CHECK(!build.isEmpty() && !research.isEmpty() && !ideas.isEmpty());
    CHECK(workspace.placeAgents(
        {QStringLiteral("renderer"), QStringLiteral("agent"), QStringLiteral("checks")}, build, 0));
    CHECK(workspace.moveAgent(QStringLiteral("service"), research));
    CHECK(workspace.setCategoryColor(research, QStringLiteral("pink")));
    CHECK(workspace.selectAgent(QStringLiteral("renderer")));
    pump(40);
    // One agent at work, two finished (not yet looked at), one waiting on an
    // answer; the rest have no status yet.
    {
        namespace wire = lapis::session::wire;
        wire::AttentionSnapshot working;
        working.available = working.connected = working.ready = true;
        working.activity = lapis::session::attention::Activity::working;
        workspace.agent(QStringLiteral("checks"))->applyAttention(working);
        auto asking = working;
        asking.requests.emplace_back();
        workspace.agent(QStringLiteral("notes"))->applyAttention(asking);
    }
    finish_turn(*workspace.agent(QStringLiteral("agent")));
    finish_turn(*workspace.agent(QStringLiteral("service")));
    pump(60);
    CHECK(!item(QStringLiteral("agentTabs"))->isVisible());

    // A row per agent, in the sidebar's order, under its category's header.
    const auto order = sidebar_ids(workspace);
    CHECK(order == (QStringList{QStringLiteral("shell"), QStringLiteral("notes"),
                                QStringLiteral("renderer"), QStringLiteral("agent"),
                                QStringLiteral("checks"), QStringLiteral("service")}));
    const auto default_row_height = item(QStringLiteral("sidebarAgent_renderer"))->height();
    qreal previous = -1;
    for (const auto& id : order) {
        auto* row = item(QStringLiteral("sidebarAgent_") + id);
        CHECK(row->isVisible() && row->height() >= 24);
        const auto top = row->mapToScene(QPointF()).y();
        CHECK(top > previous);
        previous = top;
    }
    CHECK(item(QStringLiteral("category_") + build)->mapToScene(QPointF()).y() <
          item(QStringLiteral("sidebarAgent_renderer"))->mapToScene(QPointF()).y());
    CHECK(item(QStringLiteral("sidebarAgent_notes"))->mapToScene(QPointF()).y() <
          item(QStringLiteral("category_") + build)->mapToScene(QPointF()).y());
    // Each row's mark says what its agent does, but only the rare ones show: a
    // working or done agent's row is its title alone, and a diamond waiting on
    // an answer sits after the title. Nothing says whether the person looked:
    // no dot on a row, no count on a category.
    const auto row_mark = [&](const QString& id) {
        auto* mark = item(QStringLiteral("sidebarStatus_") + id);
        return std::pair{mark->property("shape").toString(),
                         mark->property("tone").value<QColor>()};
    };
    CHECK(row_mark(QStringLiteral("checks")).first == QStringLiteral("dot"));
    CHECK(row_mark(QStringLiteral("checks")).second ==
          window->property("activityColor").value<QColor>());
    CHECK(row_mark(QStringLiteral("agent")).first == QStringLiteral("ring"));
    CHECK(row_mark(QStringLiteral("notes")).first == QStringLiteral("diamond"));
    CHECK(row_mark(QStringLiteral("notes")).second ==
          window->property("attentionColor").value<QColor>());
    CHECK(!item(QStringLiteral("sidebarStatus_checks"))->isVisible());
    CHECK(!item(QStringLiteral("sidebarStatus_agent"))->isVisible());
    {
        auto* mark = item(QStringLiteral("sidebarStatus_notes"));
        auto* title = item(QStringLiteral("sidebarTitle_notes"));
        CHECK(mark->isVisible());
        CHECK(mark->mapToScene(QPointF()).x() >= title->mapToScene(QPointF(title->width(), 0)).x());
    }
    CHECK(workspace.agent(QStringLiteral("agent"))->unseen() &&
          workspace.agent(QStringLiteral("service"))->unseen());
    for (const auto& id : sidebar_ids(workspace))
        CHECK(find_visual(window->contentItem(), QStringLiteral("sidebarUnseen_") + id) == nullptr);
    for (const auto& value : workspace.categories())
        CHECK(find_visual(window->contentItem(),
                          QStringLiteral("categoryUnseen_") +
                              value.toMap().value(QStringLiteral("id")).toString()) == nullptr);
    // The mark grows with the sidebar's text.
    const auto default_mark = item(QStringLiteral("sidebarStatus_notes"))->width();
    CHECK(default_mark >= 8);
    // The focused row is marked in its category's colour; the others are not.
    const auto hex = [&](const QString& category) {
        for (const auto& value : workspace.categories())
            if (value.toMap().value(QStringLiteral("id")).toString() == category)
                return call_window(*window, "categoryHex",
                                   value.toMap().value(QStringLiteral("color")).toString())
                    .value<QColor>();
        return QColor();
    };
    // Row colours ease (the window's colorDuration); read them settled.
    const auto settled = [&] { pump(window->property("colorDuration").toInt() + 80); };
    const auto edge = [&](const QString& id) {
        settled();
        return QQmlProperty::read(item(QStringLiteral("sidebarAgentEdge_") + id),
                                  QStringLiteral("color"))
            .value<QColor>();
    };
    const auto marked = [&](const QString& id) {
        return item(QStringLiteral("sidebarAgent_") + id)->property("marked").toBool();
    };
    CHECK(marked(QStringLiteral("renderer")) && !marked(QStringLiteral("agent")));
    CHECK(edge(QStringLiteral("renderer")) == hex(build));
    CHECK(edge(QStringLiteral("agent")).alphaF() < 0.9);
    CHECK(hex(research) ==
          call_window(*window, "categoryHex", QStringLiteral("pink")).value<QColor>());
    CHECK(hex(build) != hex(research));
    capture("default");

    // A click on a row in another category shows that agent and its category,
    // and leaves the keyboard with the terminal.
    wait_terminal_focus(*window, *terminal);
    click_visual(*window, *item(QStringLiteral("sidebarAgent_service")));
    pump(40);
    CHECK(focused_id(workspace) == QStringLiteral("service"));
    CHECK(workspace.activeCategoryId() == research);
    CHECK(terminal->document() == workspace.agent(QStringLiteral("service")));
    CHECK(terminal->hasActiveFocus());
    CHECK(!workspace.agent(QStringLiteral("service"))->unseen());
    // Looked at or not, a finished agent's mark is the same.
    CHECK(workspace.agent(QStringLiteral("agent"))->unseen());
    CHECK(row_mark(QStringLiteral("service")) == row_mark(QStringLiteral("agent")));
    capture("seen-and-unseen");
    CHECK(marked(QStringLiteral("service")) && !marked(QStringLiteral("renderer")));
    CHECK(edge(QStringLiteral("service")) == hex(research));
    // A header click selects its category and the agent it remembers.
    click_visual(*window, *item(QStringLiteral("category_") + build));
    pump(40);
    CHECK(workspace.activeCategoryId() == build &&
          focused_id(workspace) == QStringLiteral("renderer"));
    // Command-Shift-] walks the rows in order, across the category edge.
    wait_input_ready(*window, *terminal);
    CHECK(workspace.selectAgent(QStringLiteral("checks")));
    pump(20);
    send_binding(*window, config_model.sequences(QStringLiteral("nextWindow")).front());
    pump(40);
    CHECK(focused_id(workspace) == QStringLiteral("service") && marked(QStringLiteral("service")));
    send_binding(*window, config_model.sequences(QStringLiteral("nextWindow")).front());
    pump(40);
    CHECK(focused_id(workspace) == QStringLiteral("shell") &&
          workspace.activeCategoryId() == QStringLiteral("general"));
    send_binding(*window, config_model.sequences(QStringLiteral("previousWindow")).front());
    pump(40);
    CHECK(focused_id(workspace) == QStringLiteral("service"));

    // A row's right-click opens the strip card's agent menu for that agent.
    auto* agent_menu = window->findChild<QObject*>(QStringLiteral("agentMenu"));
    CHECK(agent_menu != nullptr);
    right_click(*window, *item(QStringLiteral("sidebarAgent_notes")));
    wait_popup(*agent_menu, true);
    CHECK(focused_id(workspace) == QStringLiteral("notes"));
    CHECK(QMetaObject::invokeMethod(agent_menu, "close"));
    wait_popup(*agent_menu, false);

    // The category menu's Colour sets the category's colour.
    auto* category_menu = window->findChild<QObject*>(QStringLiteral("categoryMenu"));
    CHECK(category_menu != nullptr);
    right_click(*window, *item(QStringLiteral("category_") + build));
    wait_popup(*category_menu, true);
    CHECK(workspace.activeCategoryId() == build);
    // The swatches are the Colour submenu's items, under its content item.
    auto* color_menu = window->findChild<QObject*>(QStringLiteral("categoryColorMenu"));
    CHECK(color_menu != nullptr);
    auto* swatches = color_menu->property("contentItem").value<QQuickItem*>();
    CHECK(swatches != nullptr);
    auto* cyan = find_visual(swatches, QStringLiteral("categoryColor_cyan"));
    CHECK(cyan != nullptr);
    CHECK(find_visual(swatches, QStringLiteral("categoryColor_grey")) != nullptr);
    CHECK(QMetaObject::invokeMethod(cyan, "click"));
    pump(40);
    if (category_menu->property("visible").toBool())
        CHECK(QMetaObject::invokeMethod(category_menu, "close"));
    wait_popup(*category_menu, false);
    CHECK(workspace.categories().at(1).toMap().value(QStringLiteral("color")).toString() ==
          QStringLiteral("cyan"));
    // The colour shows in the group's name: full for the shown group, the
    // quieter mix for the others. No number or square precedes the name.
    const auto name_color = [&](const QString& category) {
        settled();
        return QQmlProperty::read(item(QStringLiteral("categoryName_") + category),
                                  QStringLiteral("color"))
            .value<QColor>();
    };
    CHECK(name_color(build) ==
          call_window(*window, "categoryHex", QStringLiteral("cyan")).value<QColor>());
    CHECK(name_color(research) ==
          call_window(*window, "categoryQuietHex", QStringLiteral("pink")).value<QColor>());
    CHECK(name_color(research) != hex(research));
    CHECK(edge(QStringLiteral("renderer")).rgb() == hex(build).rgb());
    {
        auto* header = item(QStringLiteral("category_") + build);
        auto* name = item(QStringLiteral("categoryName_") + build);
        auto* title = item(QStringLiteral("sidebarTitle_renderer"));
        // A little in from the panel's edge (sidebarInset), not a rail's tight margin.
        CHECK(name->mapToItem(header, QPointF()).x() <=
              window->property("sidebarInset").toInt() + 1);
        CHECK(qAbs(name->mapToScene(QPointF()).x() - title->mapToScene(QPointF()).x()) <= 1);
    }

    // A row dragged onto another category's header moves the agent there.
    wait_input_ready(*window, *terminal);
    auto* target = item(QStringLiteral("category_") + ideas);
    drag_visual(*window, *item(QStringLiteral("sidebarAgent_notes")),
                target->mapToScene(QPointF(target->width() / 2, target->height() / 2)));
    pump(40);
    CHECK(workspace.agentPlace(QStringLiteral("notes"))
              .value(QStringLiteral("category"))
              .toString() == QStringLiteral("Ideas"));
    CHECK(item(QStringLiteral("sidebarAgent_notes"))->mapToScene(QPointF()).y() >
          target->mapToScene(QPointF()).y());

    // Captures for review: themes, a narrow sidebar with long titles, the
    // owner's larger text, and the strip shown. Each waits out the theme's
    // colour transitions before it is taken.
    CHECK(workspace.selectAgent(QStringLiteral("renderer")));
    const auto settle = [] { pump(400); };
    CHECK(config_model.setTheme(QStringLiteral("oled")));
    settle();
    capture("oled");
    CHECK(config_model.setTheme(QStringLiteral("daylight")));
    settle();
    capture("daylight");
    CHECK(config_model.setTheme(QStringLiteral("lapis")));
    CHECK(config_model.setTerminalFontSize(21));
    settle();
    auto* rail = item(QStringLiteral("categoryRail"));
    CHECK(rail->width() > 196);
    // Larger terminal text makes the sidebar's rows and names larger too.
    CHECK(item(QStringLiteral("sidebarAgent_renderer"))->height() > default_row_height);
    CHECK(item(QStringLiteral("sidebarStatus_notes"))->width() > default_mark);
    capture("comfortable-font21");
    // The owner's setting: the oled theme at terminal font 21.
    CHECK(config_model.setTheme(QStringLiteral("oled")));
    settle();
    capture("oled-font21");
    CHECK(config_model.setTheme(QStringLiteral("lapis")));
    settle();
    CHECK(config_model.setTerminalFontSize(kTerminalFontSizeDefault));
    CHECK(config_model.setPreviewsVisible(true));
    settle();
    CHECK(item(QStringLiteral("agentTabs"))->isVisible());
    capture("strip");
    CHECK(config_model.setPreviewsVisible(false));
    CHECK(config_model.setDensity(QStringLiteral("minimal")));
    CHECK(workspace.renameAgent(QStringLiteral("renderer"),
                                QStringLiteral("Refactor the renderer's glyph cache and atlas")));
    CHECK(workspace.renameAgent(QStringLiteral("agent"),
                                QStringLiteral("Review the adapter contract before continuing")));
    CHECK(workspace.renameCategory(research, QStringLiteral("Research and long-range planning")));
    settle();
    // Long names elide inside the sidebar.
    auto* long_row = item(QStringLiteral("sidebarAgent_renderer"));
    CHECK(scene_rect(*rail).contains(scene_rect(*long_row)));
    capture("narrow-long");
    CHECK(config_model.setDensity(QStringLiteral("comfortable")));
    pump(40);
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
        target: app
        function onDiagnosticsChanged() { throw new Error("diagnostics fixture warning") }
    }
    Timer { interval: 1; running: true; onTriggered: warningWindow.warn() }
}
)"));
    lapis::desktop::Workspace workspace(lapis::desktop::WorkspaceMode::fixture);
    lapis::desktop::AppWindow preview(
        workspace, {.source = QUrl::fromLocalFile(path), .compact = true, .screen = QString()});
    int notifications = 0;
    QObject observation;
    QObject::connect(&preview, &lapis::desktop::AppWindow::diagnosticsChanged, &observation,
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

// History through the window, with a real agent: a wide translucent bar down
// the terminal's right edge that the whole strip answers, a press on the
// thumb that keeps its hold, the wheel over the bar still scrolling, and
// letting go at the bottom returning to live.
int run_history_ui_tests() {
    using namespace lapis::desktop;
    QTemporaryDir directory(QStringLiteral("/tmp/lapis-history-ui-XXXXXX"));
    CHECK(directory.isValid());
    const QDir root(QFileInfo(directory.path()).canonicalFilePath());
    CHECK(root.mkpath(QStringLiteral("bin")) && root.mkpath(QStringLiteral("project")));
    {
        QFile script(root.filePath(QStringLiteral("bin/grok")));
        CHECK(script.open(QIODevice::WriteOnly));
        script.write("#!/bin/sh\n"
                     "i=0\n"
                     "while [ $i -lt 400 ]; do printf 'line %04d\\n' $i; i=$((i + 1)); done\n"
                     "echo all printed\n"
                     "exec sleep 600\n");
        script.close();
        CHECK(script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    }
    const auto path = qgetenv("PATH");
    qputenv("PATH", QFile::encodeName(root.filePath(QStringLiteral("bin"))) + ':' + path);
    const auto restore_path = qScopeGuard([&path] { qputenv("PATH", path); });
    WorkspaceOptions options;
    options.storagePath = root.filePath(QStringLiteral("workspace.json"));
    Workspace workspace(WorkspaceMode::live, options);
    Config config_model;
    config_model.setSourcePathForTesting(root.filePath(QStringLiteral("lapis.json")));
    AppWindow preview(workspace, {.source = QUrl::fromLocalFile(QStringLiteral(LAPIS_QML_SOURCE)),
                                  .compact = false,
                                  .screen = QString(),
                                  .config = &config_model});
    CHECK(preview.load());
    auto* window = preview.window();
    window->resize(1200, 800);
    wait_active(*window);
    const auto item = [window](const QString& name) {
        auto* found = find_visual(window->contentItem(), name);
        if (found == nullptr)
            throw std::runtime_error("missing item " + name.toStdString());
        return found;
    };
    const auto wait = [](const std::function<bool()>& done) {
        QElapsedTimer elapsed;
        elapsed.start();
        while (!done() && elapsed.elapsed() < 15000)
            pump(20);
        return done();
    };
    CHECK(workspace.createAgent(root.filePath(QStringLiteral("project")), QStringLiteral("scroll"),
                                QStringLiteral("grok")));
    auto* agent = workspace.focusedAgent();
    CHECK(agent != nullptr);
    CHECK(wait([agent] {
        return agent->inputReady() &&
               screen_text(agent->snapshot()).contains(QStringLiteral("all printed"));
    }));
    // Exercise QML-to-C++ URL conversion and shell quoting together. No file
    // access or OS drop is needed to preserve the network authority.
    const QVariant dropUrls = QVariantList{
        QStringLiteral("file://server/share/a%20b%27c"), QStringLiteral("file:///local/%E7%95%8C"),
        QStringLiteral("https://example.com/file"), QStringLiteral("file:///invalid%ZZ")};
    QVariant droppedPaths;
    CHECK(QMetaObject::invokeMethod(window, "quoteDroppedPaths",
                                    Q_RETURN_ARG(QVariant, droppedPaths),
                                    Q_ARG(QVariant, dropUrls)));
    CHECK(droppedPaths.toString() == QStringLiteral("'//server/share/a b'\\''c' '/local/界' "));
    const auto settled = [agent] {
        return agent->historyActive() && !agent->historyRequestPending();
    };
    agent->scrollHistory(3);
    CHECK(wait(settled));
    pump(100);
    // Each row shows once across the join of kept rows and the screen, even
    // after the window's size brought kept rows back onto the screen.
    int previous = -1;
    int lines = 0;
    for (const auto& row : screen_text(agent->snapshot()).split(QLatin1Char('\n'))) {
        if (!row.trimmed().startsWith(QStringLiteral("line ")))
            continue;
        const int number = row.trimmed().mid(5, 4).toInt();
        if (previous >= 0 && number != previous + 1)
            throw std::runtime_error("History seam jumped from " + std::to_string(previous) +
                                     " to " + std::to_string(number) + " in " +
                                     std::to_string(agent->snapshot().size.rows) +
                                     " rows: " + screen_text(agent->snapshot()).toStdString());
        previous = number;
        ++lines;
    }
    CHECK(lines > 3);
    auto* terminal = item(QStringLiteral("liveTerminal"));
    auto* bar = item(QStringLiteral("historyScrubber"));
    auto* track = item(QStringLiteral("historyTrack"));
    auto* thumb = item(QStringLiteral("historyThumb"));
    CHECK(bar->isVisible() && bar->width() >= 32 && track->width() >= 12);
    CHECK(std::abs(scene_rect(*bar).right() - scene_rect(*terminal).right()) < 1 &&
          std::abs(scene_rect(*bar).height() - scene_rect(*terminal).height()) < 1);
    if (const auto prefix = qEnvironmentVariable("LAPIS_WORKSPACE_CAPTURE_PREFIX");
        !prefix.isEmpty())
        CHECK(window->grabWindow().save(prefix + QStringLiteral("history.png")));
    static quint64 stamp = 5'000'000;
    const auto mouse = [window](QEvent::Type type, QPointF at, Qt::MouseButtons buttons) {
        QMouseEvent event(type, at, window->mapToGlobal(at),
                          type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton, buttons,
                          Qt::NoModifier);
        event.setTimestamp(stamp += 16);
        QCoreApplication::sendEvent(window, &event);
        pump(10);
    };
    // The pointer anywhere over the strip, even well left of the track, lights it.
    const auto idle_width = track->width();
    const auto strip_left = scene_rect(*bar).left() + 3;
    mouse(QEvent::MouseMove, QPointF(strip_left, scene_rect(*bar).center().y()), Qt::NoButton);
    pump(300);
    CHECK(track->width() > idle_width);
    // A press on the thumb near its top keeps that hold as it moves.
    const auto held = scene_rect(*thumb).top() + 6;
    const auto thumb_top = scene_rect(*thumb).top();
    mouse(QEvent::MouseButtonPress, QPointF(strip_left, held), Qt::LeftButton);
    CHECK(std::abs(scene_rect(*thumb).top() - thumb_top) < 2);
    for (int step = 1; step <= 6; ++step)
        mouse(QEvent::MouseMove, QPointF(strip_left, held - step * 20), Qt::LeftButton);
    CHECK(std::abs(scene_rect(*thumb).top() - (thumb_top - 120)) < 2);
    mouse(QEvent::MouseButtonRelease, QPointF(strip_left, held - 120), Qt::NoButton);
    CHECK(wait(settled) && agent->historyActive());
    // The wheel over the bar still scrolls the terminal.
    const auto before = agent->historyPosition();
    const auto over = QPointF(scene_rect(*bar).center());
    QWheelEvent wheel(over, window->mapToGlobal(over), QPoint(), QPoint(0, 120), Qt::NoButton,
                      Qt::NoModifier, Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(window, &wheel);
    CHECK(wait(settled) && agent->historyPosition() < before);
    // Letting go with the thumb at the bottom is live again.
    const auto grip = scene_rect(*thumb).center();
    mouse(QEvent::MouseButtonPress, grip, Qt::LeftButton);
    for (int step = 1; step <= 8; ++step)
        mouse(QEvent::MouseMove, grip + QPointF(0, step * 200), Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease, grip + QPointF(0, 1600), Qt::NoButton);
    CHECK(wait([agent] { return !agent->historyActive(); }) && !bar->isVisible());
    CHECK(workspace.closeAgent(agent->agentId()));
    CHECK(wait([&workspace] { return workspace.agents().isEmpty(); }));
    return EXIT_SUCCESS;
}

// A restarted window brings back the guess at an agent's cursor: the state
// file an earlier window wrote is read, the conversation (a stand-in helper)
// is still at the offer's turn, and the guess is drawn and reported seen. The
// window's own choices come back too.
int run_restored_suggestion_tests() {
    using namespace lapis::desktop;
    QTemporaryDir config(QStringLiteral("/tmp/lapis-ui-XXXXXX"));
    CHECK(config.isValid());
    const QDir root(config.path());
    CHECK(root.mkpath(QStringLiteral("bin")));
    const auto python = root.filePath(QStringLiteral("bin/python3"));
    {
        QFile script(python);
        CHECK(script.open(QIODevice::WriteOnly));
        script.write("#!/bin/sh\nprintf '%s' '{\"conversation\":\"c\",\"turn\":3}'\n");
    }
    CHECK(QFile::setPermissions(python, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    Workspace workspace(WorkspaceMode::fixture);
    auto* agent = workspace.agent(QStringLiteral("agent"));
    CHECK(agent != nullptr);
    agent->setHarnessId(QStringLiteral("claude"));
    lapis::session::wire::AttentionSnapshot finished;
    finished.available = finished.connected = finished.ready = true;
    finished.activity = lapis::session::attention::Activity::turn_completed;
    agent->applyAttention(finished);
    CHECK(workspace.selectAgent(QStringLiteral("agent")));
    NextPrompt next(
        [](const QString& id) -> std::optional<NextPrompt::Agent> {
            if (id != QLatin1String("agent"))
                return std::nullopt;
            return NextPrompt::Agent{{},
                                     QStringLiteral("~/x"),
                                     QStringLiteral("claude"),
                                     QStringLiteral("c"),
                                     QStringLiteral("agent"),
                                     QStringLiteral("general"),
                                     QStringLiteral("> |")};
        },
        [] { return QJsonArray{}; },
        [&root](const QString& name) { return root.filePath(QStringLiteral("bin/") + name); },
        {root.filePath(QStringLiteral("runtime")),
         root.filePath(QStringLiteral("runtime/next_prompt.jsonl"))});
    NextPromptSettings on;
    on.automatic = true;
    next.setSettings(on);
    const auto path = root.filePath(QStringLiteral("runtime/gui_state.json"));
    {
        QFile file(path);
        CHECK(file.open(QIODevice::WriteOnly));
        file.write(R"({"version": 1, "sections": {
            "window": {"lastHarness": "codex", "lastMode": "edits",
                "lastModels": {"codex": 7, "claude": "custom"},
                "sideTerminalOpen": true},
            "nextPrompt": {"offers": {"agent": {"key": "agent:earlier.1",
                "text": "run the tests", "conversation": "c", "turn": 3, "seenMs": 0}}}}})");
    }
    GuiState state(path);
    CHECK(state.load());
    next.restoreState(state.section(QStringLiteral("nextPrompt")).toObject());
    AppWindow preview(workspace, {.source = QUrl::fromLocalFile(QStringLiteral(LAPIS_QML_SOURCE)),
                                  .compact = false,
                                  .screen = QString(),
                                  .nextPrompt = &next,
                                  .guiState = &state});
    CHECK(preview.load());
    auto* window = preview.window();
    window->resize(1400, 960);
    wait_active(*window);
    CHECK(window->property("lastHarness").toString() == QStringLiteral("codex") &&
          window->property("lastMode").toString() == QStringLiteral("edits"));
    CHECK(workspace.selectAgent(QStringLiteral("checks")));
    pump(2);
    CHECK(workspace.selectAgent(QStringLiteral("agent")));
    pump(2);
    const auto remembered = window->property("lastModels").toMap();
    CHECK(remembered.value(QStringLiteral("claude")).toString() == QLatin1String("custom") &&
          !remembered.contains(QStringLiteral("codex")) &&
          !remembered.contains(QStringLiteral("ghost")));
    auto* terminal = qobject_cast<TerminalSurface*>(
        find_visual(window->contentItem(), QStringLiteral("liveTerminal")));
    CHECK(terminal != nullptr && terminal->document() == agent);
    CHECK(pump_until([&] { return terminal->suggestion() == QStringLiteral("run the tests"); },
                     5000));
    CHECK(terminal->suggestionKey() == QStringLiteral("agent:earlier.1"));
    // Drawn: on screen, it is reported seen under the restored offer's key.
    CHECK(pump_until([&] { return next.readyAgents().value(QStringLiteral("agent")).toBool(); },
                     5000));
    pump(60);
    const auto shown = window->grabWindow();
    if (const auto prefix = qEnvironmentVariable("LAPIS_WORKSPACE_CAPTURE_PREFIX");
        !prefix.isEmpty())
        CHECK(shown.save(prefix + QStringLiteral("restored-suggestion.png")));
    next.used(QStringLiteral("agent"), false, 0, QStringLiteral("agent:earlier.1"));
    CHECK(pump_until([&] { return terminal->suggestion().isEmpty(); }, 2000));
    pump(60);
    const auto plain = window->grabWindow();
    CHECK(shown.size() == plain.size() && shown != plain);
    // A choice made now is kept for the next window.
    window->setProperty("lastHarness", QStringLiteral("claude"));
    CHECK(state.value(QStringLiteral("lastHarness")).toString() == QStringLiteral("claude"));
    return EXIT_SUCCESS;
}

// A made-up conversation for the window captures: a prompt and a reply with
// inline code, a list and a code block.
const char* const kSettingsThread[] = {
    R"({"type":"user","uuid":"p1","timestamp":"2026-11-11T16:00:00.000Z","message":{"content":"Why is the warm switch slow?"}})",
    R"({"type":"assistant","uuid":"a1","timestamp":"2026-11-11T16:00:20.000Z","message":{"content":[{"type":"text","text":"Each switch measured the whole screen again. Two changes fix it:\n\n- keep the last `layout()` result per agent\n- skip glyph shaping for rows that did not change\n\n```cpp\nif (auto hit = cache.find(id); hit != cache.end())\n    return hit->second;\n```\n\nThe warm switch drops from 18 ms to 4 ms."}]}})",
    R"({"type":"system","subtype":"turn_duration","durationMs":20000})",
};

// Light mode: the sun and moon beside the visual view and the Commands entry
// switch the window and the thread together, terminals untouched.
void check_appearance_toggle(QQuickWindow& window, lapis::desktop::Workspace& workspace,
                             lapis::desktop::Config& config_model, const QTemporaryDir& directory,
                             const QString& captures) {
    using namespace lapis::desktop;
    auto* toggle = find_visual(window.contentItem(), QStringLiteral("appearanceToggle"));
    CHECK(toggle != nullptr && toggle->isVisible());
    const auto terminal_background = [&window] {
        return qobject_cast<TerminalSurface*>(
                   find_visual(window.contentItem(), QStringLiteral("liveTerminal")))
            ->property("backgroundColor")
            .value<QColor>();
    };
    const QColor terminal_before = terminal_background();
    CHECK(config_model.appearance() == QStringLiteral("dark") &&
          config_model.themeName() == QStringLiteral("oled"));
    click_visual(window, *toggle);
    pump(50);
    CHECK(config_model.appearance() == QStringLiteral("light"));
    CHECK(config_model.themeName() == QStringLiteral("daylight"));
    CHECK(window.property("backgroundColor").value<QColor>() == QColor(QStringLiteral("#f4f5f7")));
    CHECK(terminal_background() == terminal_before);
    auto* view = find_visual(window.contentItem(), QStringLiteral("visualView"));
    CHECK(view != nullptr);
    CHECK(view->property("themeName").toString() == QStringLiteral("claude-light"));
    CHECK(view->property("lightPage").toBool());

    // The Commands entry names the way it switches.
    CHECK(QMetaObject::invokeMethod(&window, "openCommandsDialog"));
    auto* commands = window.findChild<QObject*>(QStringLiteral("commandsDialog"));
    CHECK(commands != nullptr);
    wait_popup(*commands, true);
    QString label;
    for (const auto& value : commands->property("commands").toList())
        if (value.toMap().value(QStringLiteral("id")).toString() ==
            QStringLiteral("toggleAppearance"))
            label = value.toMap().value(QStringLiteral("label")).toString();
    CHECK(label == QStringLiteral("Switch to dark mode"));
    CHECK(QMetaObject::invokeMethod(commands, "close"));
    wait_popup(*commands, false);

    // The whole window in light mode with a thread on the stage.
    if (!captures.isEmpty()) {
        const auto path = directory.filePath(QStringLiteral("thread.jsonl"));
        QFile file(path);
        CHECK(file.open(QIODevice::WriteOnly));
        for (const char* line : kSettingsThread)
            file.write(QByteArray(line) + '\n');
        file.close();
        auto* model = qobject_cast<TranscriptModel*>(workspace.transcript());
        CHECK(model != nullptr);
        model->setPath(path);
        model->loadNow();
        view->setProperty("visible", true);
        pump(400);
        CHECK(
            window.grabWindow().save(QDir(captures).filePath(QStringLiteral("window-light.png"))));
        view->setProperty("visible", false);
        model->setPath(QString());
    }
    CHECK(QMetaObject::invokeMethod(&window, "runCommand",
                                    Q_ARG(QVariant, QStringLiteral("toggleAppearance"))));
    pump(50);
    CHECK(config_model.appearance() == QStringLiteral("dark") &&
          config_model.themeName() == QStringLiteral("oled"));
    CHECK(view->property("themeName").toString() == QStringLiteral("terminal"));
}

// The Settings window: sections by click and by arrow, search across
// sections (keys included), Reset on a row and for a section, the light and
// dark switch with its preview, and Esc. Captures go to `captures`.
void check_settings_window(QQuickWindow& window, lapis::desktop::AppWindow& preview,
                           lapis::desktop::Config& config_model, const QString& captures) {
    using namespace lapis::desktop;
    auto* dialog = window.findChild<QObject*>(QStringLiteral("settingsDialog"));
    CHECK(dialog != nullptr);
    const auto save = [&](const char* name) {
        if (!captures.isEmpty())
            CHECK(window.grabWindow().save(QDir(captures).filePath(QString::fromLatin1(name))));
    };
    const auto item = [&window](const QString& name) {
        return find_visual(window.contentItem(), name);
    };
    const auto shown = [&item](const QString& name) {
        auto* found = item(name);
        return found != nullptr && found->isVisible();
    };
    const auto search = [&](const QString& text) {
        item(QStringLiteral("settingsSearch"))->setProperty("text", text);
        pump(30);
    };
    CHECK(preview.openSettings());
    wait_popup(*dialog, true);
    CHECK(dialog->property("title").toString() == QStringLiteral("Settings"));
    CHECK(item(QStringLiteral("settingsSearch"))->hasActiveFocus());
    CHECK(dialog->property("currentSection").toString() == QStringLiteral("general"));
    CHECK(shown(QStringLiteral("switch-keepAwake")) && !shown(QStringLiteral("fontLarger")));
    CHECK(!item(QStringLiteral("resetSection"))->isEnabled()); // nothing changed yet
    pump(100);
    save("settings-general.png");

    // Down from the search reaches the sections; arrows walk them.
    QKeyEvent down(QEvent::KeyPress, Qt::Key_Down, Qt::NoModifier);
    QCoreApplication::sendEvent(&window, &down);
    pump(20);
    CHECK(item(QStringLiteral("settingsSections"))->hasActiveFocus());
    QCoreApplication::sendEvent(&window, &down);
    pump(20);
    CHECK(dialog->property("currentSection").toString() == QStringLiteral("appearance"));
    CHECK(shown(QStringLiteral("appearance-light")) && !shown(QStringLiteral("switch-keepAwake")));
    QKeyEvent up(QEvent::KeyPress, Qt::Key_Up, Qt::NoModifier);
    QCoreApplication::sendEvent(&window, &up);
    pump(20);
    CHECK(dialog->property("currentSection").toString() == QStringLiteral("general"));

    // Search: rows of every section by label, help or keywords, under
    // their section's name; keys too; and a line when nothing matches.
    search(QStringLiteral("font"));
    CHECK(shown(QStringLiteral("fontLarger")) && shown(QStringLiteral("visual-font-system")) &&
          shown(QStringLiteral("visual-codeFont-system")));
    CHECK(!shown(QStringLiteral("switch-keepAwake")));
    pump(100);
    save("settings-search.png");
    search(QStringLiteral("sleep"));
    CHECK(shown(QStringLiteral("switch-keepAwake")) && !shown(QStringLiteral("fontLarger")));
    search(QStringLiteral("new agent"));
    bool key_found = false;
    for (const auto& value : dialog->property("shortcutRows").toList()) {
        const auto label = value.toMap().value(QStringLiteral("label")).toString();
        if (label == QStringLiteral("New agent"))
            key_found = shown(QStringLiteral("key-") + label);
    }
    CHECK(key_found);
    search(QStringLiteral("no such setting here"));
    CHECK(shown(QStringLiteral("settingsNoMatch")));
    search(QString());
    CHECK(!shown(QStringLiteral("settingsNoMatch")) && shown(QStringLiteral("switch-keepAwake")));

    // A changed row offers Reset; Reset puts the built-in value back.
    CHECK(config_model.keepAwake() && !shown(QStringLiteral("reset-Keep this Mac awake")));
    click_setting(window, QStringLiteral("switch-keepAwake"));
    CHECK(!config_model.keepAwake());
    CHECK(shown(QStringLiteral("reset-Keep this Mac awake")));
    click_setting(window, QStringLiteral("reset-Keep this Mac awake"));
    CHECK(config_model.keepAwake() && !shown(QStringLiteral("reset-Keep this Mac awake")));
    // Reset Terminal puts every changed row of the section back.
    click_setting(window, QStringLiteral("fontLarger"));
    click_setting(window, QStringLiteral("switch-scrollToBottom"));
    CHECK(config_model.terminalFontSize() == kTerminalFontSizeDefault + 1 &&
          !config_model.scrollToBottomOnTyping());
    CHECK(dialog->property("currentSection").toString() == QStringLiteral("terminal"));
    click_setting(window, QStringLiteral("resetSection"));
    CHECK(config_model.terminalFontSize() == kTerminalFontSizeDefault &&
          config_model.scrollToBottomOnTyping());

    // Appearance leads with light, dark and Match macOS, over a preview.
    CHECK(QMetaObject::invokeMethod(dialog, "showSection",
                                    Q_ARG(QVariant, QStringLiteral("appearance"))));
    pump(100);
    auto* sample = item(QStringLiteral("settingsPreview"));
    CHECK(sample != nullptr && sample->isVisible());
    const auto page = [sample] {
        return QQmlProperty::read(sample, QStringLiteral("color")).value<QColor>();
    };
    CHECK(page().lightnessF() < 0.5);
    save("settings-appearance-dark.png");
    click_setting(window, QStringLiteral("appearance-light"));
    CHECK(config_model.appearance() == QStringLiteral("light") &&
          config_model.themeName() == QStringLiteral("daylight"));
    CHECK(page().lightnessF() > 0.5);
    CHECK(dialog->property("paletteWindow").value<QColor>().lightnessF() > 0.5);
    pump(100);
    save("settings-appearance-light.png");
    click_setting(window, QStringLiteral("appearance-system"));
    CHECK(config_model.appearance() == QStringLiteral("system") && !config_model.lightShown());
    config_model.setSystemDark(false);
    pump(30);
    CHECK(page().lightnessF() > 0.5);
    config_model.setSystemDark(true);
    pump(30);
    CHECK(page().lightnessF() < 0.5);
    click_setting(window, QStringLiteral("appearance-dark"));
    CHECK(config_model.appearance() == QStringLiteral("dark") &&
          config_model.themeName() == QStringLiteral("oled"));

    // New agents: a CLI's updates turn off and on.
    CHECK(QMetaObject::invokeMethod(dialog, "showSection",
                                    Q_ARG(QVariant, QStringLiteral("agents"))));
    pump(30);
    click_setting(window, QStringLiteral("agent-mode-edits"));
    CHECK(config_model.agentDefaults().mode == QStringLiteral("edits"));
    click_setting(window, QStringLiteral("reset-Approval mode"));
    CHECK(config_model.agentDefaults().mode.isEmpty());

    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QCoreApplication::sendEvent(&window, &escape);
    wait_popup(*dialog, false);
}

// Beside a thread the sidebar, the window around the stage and the stage's
// frame take the thread's theme (its recessed code tone for the panel, its
// page for the stage, its text roles); with a terminal showing they keep the
// window theme. A change of palette lands at once, with no eased half-way
// colour, so each check reads right after the change. Captures, when asked
// for, show the sidebar beside a thread in three themes and beside a terminal.
void check_sidebar_theme(QQuickWindow& window, lapis::desktop::Workspace& workspace,
                         lapis::desktop::Config& config_model, const QTemporaryDir& directory,
                         const QString& captures) {
    using namespace lapis::desktop;
    const auto item = [&window](const QString& name) {
        auto* found = find_visual(window.contentItem(), name);
        if (found == nullptr)
            throw std::runtime_error("missing item " + name.toStdString());
        return found;
    };
    const auto color = [](QObject* object, const char* name) {
        return QQmlProperty::read(object, QString::fromLatin1(name)).value<QColor>();
    };
    auto* rail = item(QStringLiteral("categoryRail"));
    auto* stage = item(QStringLiteral("focusedPane"));
    auto* view = item(QStringLiteral("visualView"));
    auto* focused = workspace.focusedAgent();
    CHECK(focused != nullptr);
    auto* title = item(QStringLiteral("sidebarTitle_") + focused->agentId());

    // The side panel's size: about 240/260/288 px by density at the default
    // text size, never over 30% of the window.
    const int rail_width = window.property("railWidth").toInt();
    CHECK(rail_width >= 240 && rail_width <= 420);
    CHECK(rail_width <= qRound(window.width() * 0.3));
    CHECK(qAbs(rail->width() - rail_width) <= 1);
    CHECK(window.property("sidebarFont").toInt() > window.property("chromeFont").toInt());
    CHECK(window.property("sidebarRowHeight").toInt() >= 26);

    // A terminal: the window theme.
    CHECK(!view->isVisible());
    CHECK(color(rail, "color") == window.property("surfaceColor").value<QColor>());
    CHECK(color(&window, "color") == window.property("backgroundColor").value<QColor>());
    CHECK(color(title, "color") == window.property("textColor").value<QColor>());

    // A thread on the stage, with some conversation in it.
    const auto path = directory.filePath(QStringLiteral("sidebar-thread.jsonl"));
    QFile file(path);
    CHECK(file.open(QIODevice::WriteOnly));
    for (const char* line : kSettingsThread)
        file.write(QByteArray(line) + '\n');
    file.close();
    auto* model = qobject_cast<TranscriptModel*>(workspace.transcript());
    CHECK(model != nullptr);
    model->setPath(path);
    model->loadNow();
    struct Expected {
        const char* theme;
        const char* panel; // the theme's code tone
        const char* page;
        const char* text;
        bool light;
    };
    const Expected themes[] = {{"codex", "#111111", "#181818", "#ececec", false},
                               {"tokyo-night", "#16161e", "#1a1b26", "#c0caf5", false},
                               {"claude-light", "#f1efe7", "#faf9f5", "#29261b", true}};
    view->setProperty("visible", true);
    for (const auto& expected : themes) {
        CHECK(config_model.setVisual(QStringLiteral("theme"), QString::fromLatin1(expected.theme)));
        // No pump: the palette is already in place.
        CHECK(color(rail, "color") == QColor(QString::fromLatin1(expected.panel)));
        CHECK(color(&window, "color") == QColor(QString::fromLatin1(expected.panel)));
        CHECK(color(stage, "color") == QColor(QString::fromLatin1(expected.page)));
        CHECK(color(title, "color") == QColor(QString::fromLatin1(expected.text)));
        CHECK((color(rail, "color").lightnessF() > 0.5) == expected.light);
        // Readable: the names stand well apart from the panel.
        CHECK(qAbs(color(title, "color").lightnessF() - color(rail, "color").lightnessF()) > 0.5);
        pump(300);
        if (!captures.isEmpty())
            CHECK(window.grabWindow().save(QDir(captures).filePath(
                QStringLiteral("sidebar-") + QString::fromLatin1(expected.theme) +
                QStringLiteral(".png"))));
    }

    // Back to a terminal: the window theme again, at once.
    CHECK(config_model.setVisual(QStringLiteral("theme"), QStringLiteral("codex")));
    view->setProperty("visible", false);
    CHECK(color(rail, "color") == window.property("surfaceColor").value<QColor>());
    CHECK(color(&window, "color") == window.property("backgroundColor").value<QColor>());
    CHECK(color(title, "color") == window.property("textColor").value<QColor>());
    pump(300);
    if (!captures.isEmpty())
        CHECK(window.grabWindow().save(
            QDir(captures).filePath(QStringLiteral("sidebar-terminal.png"))));
    model->setPath(QString());
    CHECK(config_model.setVisual(QStringLiteral("theme"), QStringLiteral("terminal")));
}

// Light mode and the Settings window, on a config of its own.
int run_settings_window_tests() {
    using namespace lapis::desktop;
    Workspace workspace(WorkspaceMode::fixture);
    QTemporaryDir directory;
    CHECK(directory.isValid());
    write_config(directory, QStringLiteral(R"({"theme":"oled"})"));
    Config config_model;
    config_model.setSourcePathForTesting(directory.filePath(QStringLiteral("lapis.json")));
    config_model.setSystemDark(true);
    CHECK(config_model.load());
    AppWindow preview(workspace, {.source = QUrl::fromLocalFile(QStringLiteral(LAPIS_QML_SOURCE)),
                                  .compact = true,
                                  .screen = QString(),
                                  .config = &config_model});
    CHECK(preview.load());
    auto* window = preview.window();
    window->resize(1400, 900);
    window->show();
    wait_active(*window);
    pump(80);
    const auto captures = qEnvironmentVariable("LAPIS_SETTINGS_CAPTURE_DIR");
    check_appearance_toggle(*window, workspace, config_model, directory, captures);
    check_sidebar_theme(*window, workspace, config_model, directory, captures);
    check_settings_window(*window, preview, config_model, captures);
    CHECK(preview.diagnostics().isEmpty());
    return EXIT_SUCCESS;
}

int main(int argc, char** argv) {
    bool background = false;
    bool shortcuts_only = false;
    bool history_only = false;
    bool preview_frame_only = false;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--background")
            background = true;
        else if (argument == "--shortcuts-only")
            shortcuts_only = true;
        else if (argument == "--history-only")
            history_only = true;
        else if (argument == "--preview-frame-only")
            preview_frame_only = true;
        else {
            std::cerr << "Usage: lapis_ui_preview_tests [--background] "
                         "[--shortcuts-only|--history-only|--preview-frame-only]\n";
            return EXIT_FAILURE;
        }
    }
    if (background) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
        QQuickWindow::setSceneGraphBackend("software");
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
    } else {
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Vulkan);
    }
    QCoreApplication::setAttribute(Qt::AA_MacDontSwapCtrlAndMeta);
    QGuiApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    QCoreApplication::setApplicationName(QStringLiteral("lapis-ui-preview-tests"));
    QCoreApplication::setOrganizationName(QStringLiteral("lapis"));
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    qmlRegisterUncreatableType<lapis::desktop::Agent>("Lapis", 1, 0, "Agent", "Owned by workspace");
    qmlRegisterType<lapis::desktop::TerminalSurface>("Lapis", 1, 0, "TerminalSurface");
    // Render-thread shutdown posts signal proxies to the GUI thread. Finish
    // their delivery and deferred deletion before destroying the application.
    const auto drain = qScopeGuard([] {
        QCoreApplication::sendPostedEvents();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    });
    try {
        CHECK(background == (QGuiApplication::platformName() == QStringLiteral("offscreen")));
        CHECK(!(shortcuts_only && history_only));
        CHECK(!(shortcuts_only && preview_frame_only) && !(history_only && preview_frame_only));
        if (preview_frame_only) {
            if (run_preview_frame_sync_tests() != EXIT_SUCCESS)
                return EXIT_FAILURE;
        } else if (history_only) {
            if (run_history_ui_tests() != EXIT_SUCCESS)
                return EXIT_FAILURE;
        } else if (shortcuts_only) {
            if (run_shortcut_focus_tests() != EXIT_SUCCESS)
                return EXIT_FAILURE;
        } else if (run_workspace_tests() != EXIT_SUCCESS || run_ui_tests() != EXIT_SUCCESS ||
                   run_diagnostics_reentrancy_test() != EXIT_SUCCESS ||
                   run_surface_tests(background) != EXIT_SUCCESS ||
                   run_preview_frame_sync_tests() != EXIT_SUCCESS ||
                   run_attention_dialog_tests() != EXIT_SUCCESS ||
                   run_attention_ui_tests() != EXIT_SUCCESS ||
                   run_strip_ui_tests() != EXIT_SUCCESS || run_sidebar_ui_tests() != EXIT_SUCCESS ||
                   run_history_ui_tests() != EXIT_SUCCESS ||
                   run_restored_suggestion_tests() != EXIT_SUCCESS ||
                   run_settings_window_tests() != EXIT_SUCCESS)
            return EXIT_FAILURE;
        std::cout << "ui_preview_test: PASS"
                  << (background ? " (offscreen/software; not native input or GPU acceptance)" : "")
                  << '\n';
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "ui_preview_test: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
