// The Tools pane renders the status layer's real fixture answers, including
// failure, timeout, missing, unprobed and stale states, without touching the
// user's cursor, keyboard or OS focus.
#include "terminal_surface.hpp"
#include "tool_status.hpp"
#include "ui_preview.hpp"
#include "workspace.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QHash>
#include <QMouseEvent>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QScopeGuard>
#include <QThread>
#include <functional>
#include <iostream>
#include <stdexcept>

namespace {
using lapis::desktop::ToolStatus;

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

void pump(int milliseconds) {
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < milliseconds) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
}

void until(const std::function<bool()>& condition, const char* message, int milliseconds = 15000) {
    QElapsedTimer clock;
    clock.start();
    while (!condition() && clock.elapsed() < milliseconds)
        pump(10);
    require(condition(), message);
}

QQuickItem* findVisual(QQuickItem* parent, const QString& name) {
    if (parent->objectName() == name)
        return parent;
    for (auto* child : parent->childItems())
        if (auto* found = findVisual(child, name))
            return found;
    return nullptr;
}

QQuickItem* requiredVisual(QQuickWindow& window, const QString& name) {
    auto* found = findVisual(window.contentItem(), name);
    require(found != nullptr, "a required Tools control is missing");
    return found;
}

QString labelText(QQuickWindow& window, const QString& name) {
    return requiredVisual(window, name)->property("text").toString();
}

void waitPopup(QObject& dialog, bool open) {
    const auto* property = open ? "opened" : "visible";
    QElapsedTimer clock;
    clock.start();
    while (dialog.property(property).toBool() != open && clock.elapsed() < 5000)
        pump(10);
    require(dialog.property(property).toBool() == open, "the Tools popup did not settle");
    pump(20);
}

void clickVisual(QQuickWindow& window, QQuickItem& target) {
    const auto position = target.mapToScene(QPointF(target.width() / 2, target.height() / 2));
    require(window.contentItem()->contains(position), "a Tools control is outside the window");
    const auto global = window.mapToGlobal(position);
    QMouseEvent press(QEvent::MouseButtonPress, position, global, Qt::LeftButton, Qt::LeftButton,
                      Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease, position, global, Qt::LeftButton, Qt::NoButton,
                        Qt::NoModifier);
    QCoreApplication::sendEvent(&window, &press);
    QCoreApplication::sendEvent(&window, &release);
    pump(20);
}

QString fixture(const QString& id) {
    static const QHash<QString, QString> programs{
        {QStringLiteral("claude"), QStringLiteral("/usr/bin/true")},
        {QStringLiteral("codex"), QStringLiteral("/usr/bin/false")},
        {QStringLiteral("opencode"), QStringLiteral("/bin/sleep")},
        {QStringLiteral("omp"), QStringLiteral("/usr/bin/true")},
        {QStringLiteral("agy"), QString()},
        {QStringLiteral("grok"), QStringLiteral("/usr/bin/true")},
        {QStringLiteral("kimi"), QStringLiteral("/usr/bin/true")},
        {QStringLiteral("gemini"), QStringLiteral("/usr/bin/true")},
    };
    return programs.value(id);
}

QVariantMap rowFor(const ToolStatus& status, const QString& id) {
    for (const auto& value : status.rows()) {
        const auto row = value.toMap();
        if (row.value(QStringLiteral("id")).toString() == id)
            return row;
    }
    return {};
}

int run() {
    using namespace lapis::desktop;
    Workspace workspace(WorkspaceMode::preview);
    ToolStatus status(&fixture);
    status.setProbeForTesting(QStringLiteral("claude"),
                              ToolStatus::Probe{.argv = {QStringLiteral("/bin/echo"),
                                                         QStringLiteral("lapis tools pane probe")},
                                                .timeoutMs = 1000});
    status.setProbeForTesting(
        QStringLiteral("codex"),
        ToolStatus::Probe{.argv = {QStringLiteral("/usr/bin/false")}, .timeoutMs = 1000});
    status.setProbeForTesting(
        QStringLiteral("opencode"),
        ToolStatus::Probe{.argv = {QStringLiteral("/bin/sleep"), QStringLiteral("30")},
                          .timeoutMs = 150});
    for (const auto* id : {"grok", "kimi", "gemini"})
        status.setProbeForTesting(QString::fromLatin1(id), ToolStatus::Probe{});

    UiPreview preview(workspace, {.source = QUrl::fromLocalFile(QStringLiteral(LAPIS_QML_SOURCE)),
                                  .compact = true,
                                  .screen = QString(),
                                  .tools = &status});
    require(preview.load(), "production QML loaded");
    auto* window = preview.window();
    require(window != nullptr, "production window exists");
    window->resize(1280, 900);
    window->show();
    pump(40);

    auto* dialog = window->findChild<QObject*>(QStringLiteral("toolsDialog"));
    require(dialog != nullptr, "Tools dialog instantiated");
    require(window->property("toolsAvailable").toBool(), "Tools pane reports availability");
    require(QMetaObject::invokeMethod(window, "openToolsDialog"), "Tools pane can open");
    waitPopup(*dialog, true);
    until(
        [&] {
            for (const auto& value : status.rows())
                if (value.toMap().value(QStringLiteral("state")).toString() ==
                    QStringLiteral("unknown"))
                    return false;
            return true;
        },
        "bounded fixture probes did not answer");

    status.setProbeForTesting(QStringLiteral("omp"), ToolStatus::Probe{});

    require(labelText(*window, QStringLiteral("toolState_claude")) == QStringLiteral("working"),
            "a successful probe reads working");
    require(labelText(*window, QStringLiteral("toolDetail_claude")) ==
                QStringLiteral("lapis tools pane probe"),
            "the pane shows the probe's own first line");
    require(labelText(*window, QStringLiteral("toolState_codex"))
                .startsWith(QStringLiteral("failed, exit 1")),
            "a failed probe shows its exit code");
    require(labelText(*window, QStringLiteral("toolState_opencode")) ==
                QStringLiteral("no answer before timeout"),
            "a timed-out probe does not invent an answer");
    require(labelText(*window, QStringLiteral("toolState_omp")) ==
                QStringLiteral("installed, nothing to probe"),
            "an installed unprobed command says so");
    require(labelText(*window, QStringLiteral("toolState_agy")) == QStringLiteral("not installed"),
            "an unresolved command says missing");
    for (const auto* id : {"claude", "codex", "opencode", "omp", "agy"}) {
        const auto name = QString::fromLatin1(id);
        require(labelText(*window, QStringLiteral("toolChecked_") + name)
                    .startsWith(QStringLiteral("checked ")),
                "each answer is timestamped");
        require(!requiredVisual(*window, QStringLiteral("toolStale_") + name)->isVisible(),
                "fresh answers are not marked stale");
    }

    const auto firstChecked =
        rowFor(status, QStringLiteral("claude")).value(QStringLiteral("checked")).toString();
    clickVisual(*window, *requiredVisual(*window, QStringLiteral("toolRerun_claude")));
    until(
        [&] {
            return rowFor(status, QStringLiteral("claude")).value(QStringLiteral("checked")) !=
                   firstChecked;
        },
        "the per-row rerun did not record a new answer");
    require(labelText(*window, QStringLiteral("toolState_claude")) == QStringLiteral("working"),
            "the rerun still reports the probe's verdict");

    status.setNowForTesting(QDateTime::currentDateTime().addMSecs(ToolStatus::kFreshMs + 1000));
    status.publishForTesting();
    pump(40);
    require(requiredVisual(*window, QStringLiteral("toolStale_claude"))->isVisible() &&
                requiredVisual(*window, QStringLiteral("toolStale_opencode"))->isVisible(),
            "aged answers are marked stale");
    require(labelText(*window, QStringLiteral("toolState_claude")) == QStringLiteral("working"),
            "an aged answer keeps its recorded verdict");

    QMetaObject::invokeMethod(dialog, "close");
    waitPopup(*dialog, false);
    std::cout << "tools_ui_test: pane rendered live, stale and failure states\n";
    return EXIT_SUCCESS;
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication::setAttribute(Qt::AA_MacDontSwapCtrlAndMeta);
    QGuiApplication application(argc, argv);
    application.setQuitOnLastWindowClosed(false);
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Vulkan);
    QCoreApplication::setApplicationName(QStringLiteral("lapis-tools-ui-tests"));
    QCoreApplication::setOrganizationName(QStringLiteral("lapis"));
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    qmlRegisterUncreatableType<lapis::desktop::SessionPreview>("Lapis", 1, 0, "SessionPreview",
                                                               "Owned by workspace");
    qmlRegisterType<lapis::desktop::TerminalSurface>("Lapis", 1, 0, "TerminalSurface");
    const auto drain = qScopeGuard([] {
        QCoreApplication::sendPostedEvents();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    });
    try {
        return run();
    } catch (const std::exception& error) {
        std::cerr << "tools_ui_test: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
