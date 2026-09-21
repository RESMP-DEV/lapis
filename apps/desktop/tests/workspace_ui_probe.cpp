#include "platform/window_activation.hpp"
#include "terminal_surface.hpp"
#include "ui_preview.hpp"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMouseEvent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QSysInfo>
#include <QTemporaryDir>
#include <QThread>
#include <QThreadPool>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <signal.h>
#include <source_location>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef Q_OS_MACOS
#include <libproc.h>
#include <sys/proc_info.h>
#include <unistd.h>
#endif

namespace {
using namespace lapis::desktop;
using namespace lapis::session;

constexpr int default_timeout_ms = 30000;

void require(bool condition, const char* message,
             std::source_location where = std::source_location::current()) {
    if (!condition)
        throw std::runtime_error(std::string(message) + " at line " + std::to_string(where.line()));
}

void pump(int milliseconds = 10) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    while (std::chrono::steady_clock::now() < deadline) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
}

template <typename Predicate>
void until(Predicate predicate, const char* message, int timeout = default_timeout_ms,
           std::source_location where = std::source_location::current()) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (!predicate()) {
        if (elapsed.elapsed() >= timeout)
            require(false, message, where);
        pump(5);
    }
}

QQuickItem* visual(QQuickItem* parent, const QString& name) {
    if (parent->objectName() == name)
        return parent;
    for (auto* child : parent->childItems())
        if (auto* found = visual(child, name))
            return found;
    return nullptr;
}

QPointF actionable_center(QQuickWindow& window, QQuickItem* target) {
    require(target && target->isVisible() && target->isEnabled(), "Control is not actionable");
    const QPointF center(target->width() / 2, target->height() / 2);
    const QPointF scene = target->mapToScene(center);
    require(window.contentItem()->contains(scene), "Control is outside the window");
    return scene;
}

void click_item(QQuickWindow& window, QQuickItem* target) {
    const QPointF position = actionable_center(window, target);
    const QPointF global = window.mapToGlobal(position);
    QMouseEvent press(QEvent::MouseButtonPress, position, global, Qt::LeftButton, Qt::LeftButton,
                      Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease, position, global, Qt::LeftButton, Qt::NoButton,
                        Qt::NoModifier);
    QCoreApplication::sendEvent(&window, &press);
    QCoreApplication::sendEvent(&window, &release);
    pump(20);
}

void click(QQuickWindow& window, const QString& name) {
    click_item(window, visual(window.contentItem(), name));
}

QQuickItem* item_with_text(QQuickItem* parent, const QString& text) {
    if (parent->property("text").toString().contains(text, Qt::CaseInsensitive) &&
        parent->isVisible() && parent->isEnabled())
        return parent;
    for (auto* child : parent->childItems())
        if (auto* found = item_with_text(child, text))
            return found;
    return nullptr;
}

void click_text(QQuickWindow& window, const QString& text) {
    click_item(window, item_with_text(window.contentItem(), text));
}

void click_setting(QQuickWindow& window, const QString& name) {
    auto* target = visual(window.contentItem(), name);
    auto* scroll = visual(window.contentItem(), QStringLiteral("settingsScroll"));
    require(target && scroll, "Settings control is missing");
    scroll->setProperty("contentY", 0);
    pump(10);
    const qreal bottom = target->mapToItem(scroll, QPointF()).y() + target->height();
    if (bottom > scroll->height())
        scroll->setProperty("contentY", bottom - scroll->height());
    pump(10);
    click(window, name);
}

void wait_popup(QObject& popup, bool opened) {
    until([&] { return popup.property(opened ? "opened" : "visible").toBool() == opened; },
          "Popup state did not settle");
    pump(30);
}

QString screen(const SessionPreview& session) {
    const auto& snapshot = session.snapshot();
    QString result;
    for (std::size_t row = 0; row < snapshot.size.rows; ++row) {
        for (std::size_t column = 0; column < snapshot.size.columns; ++column) {
            const auto grapheme = snapshot.text(row * snapshot.size.columns + column);
            result += QString::fromUcs4(grapheme.data(), static_cast<qsizetype>(grapheme.size()));
        }
        result += QLatin1Char('\n');
    }
    return result;
}

void send(SessionPreview& session, const QByteArray& command) {
    require(session.inputReady(), "Session input is not ready");
    session.sendText(command + '\r');
}

struct ChildProcesses {
    qint64 shell{};
    qint64 service{};
    bool operator==(const ChildProcesses&) const = default;
};

ChildProcesses process_probe(SessionPreview& session, const QByteArray& marker) {
    send(session, "PS1=; PS2=; unset PROMPT_COMMAND; stty -echo; printf '\\033[2J\\033[H" + marker +
                      ":%s:%s:END\\n' \"$$\" \"$PPID\"");
    const QRegularExpression ids(QLatin1String("^") +
                                     QRegularExpression::escape(QString::fromLatin1(marker)) +
                                     QStringLiteral(":(\\d+):(\\d+):END$"),
                                 QRegularExpression::MultilineOption);
    QRegularExpressionMatch match;
    until(
        [&] {
            match = ids.match(screen(session));
            return match.hasMatch();
        },
        "Observed shell process IDs timed out");
    return {match.captured(1).toLongLong(), match.captured(2).toLongLong()};
}

bool process_gone(qint64 pid) {
    return pid != 0 && ::kill(static_cast<pid_t>(pid), 0) == -1 && errno == ESRCH;
}

bool terminate(qint64 pid) { return pid != 0 && ::kill(static_cast<pid_t>(pid), SIGTERM) == 0; }

bool kill(qint64 pid) { return pid != 0 && ::kill(static_cast<pid_t>(pid), SIGKILL) == 0; }

struct SessionRecord {
    QString id;
    ChildProcesses child;
    QString endpoint;
};

#ifdef Q_OS_MACOS
std::vector<qint64> socket_owner_pids(const QString& endpoint) {
    std::vector<qint64> owners;
    std::vector<pid_t> pids(8192);
    const int pid_count =
        ::proc_listallpids(pids.data(), static_cast<int>(pids.size() * sizeof(pid_t)));
    if (pid_count <= 0)
        return owners;
    pids.resize(static_cast<std::size_t>(pid_count));
    const QByteArray wanted = QFile::encodeName(endpoint);
    std::vector<struct proc_fdinfo> descriptors;
    for (pid_t pid : pids) {
        char executable[PROC_PIDPATHINFO_MAXSIZE]{};
        if (pid == ::getpid() || ::proc_pidpath(pid, executable, sizeof(executable)) <= 0 ||
            !QByteArray(executable).endsWith("/lapis_session_service"))
            continue;
        int byte_count = ::proc_pidinfo(pid, PROC_PIDLISTFDS, 0, nullptr, 0);
        if (byte_count <= 0)
            continue;
        const auto descriptor_count = static_cast<std::size_t>(byte_count) / sizeof(descriptors[0]);
        if (descriptor_count > 4096)
            continue;
        descriptors.assign(descriptor_count, {});
        byte_count = ::proc_pidinfo(pid, PROC_PIDLISTFDS, 0, descriptors.data(),
                                    static_cast<int>(descriptors.size() * sizeof(descriptors[0])));
        if (byte_count <= 0)
            continue;
        const auto returned = static_cast<std::size_t>(byte_count) / sizeof(descriptors[0]);
        for (std::size_t index = 0; index < returned; ++index) {
            if (descriptors[index].proc_fdtype != PROX_FDTYPE_SOCKET)
                continue;
            struct socket_fdinfo socket{};
            if (::proc_pidfdinfo(pid, descriptors[index].proc_fd, PROC_PIDFDSOCKETINFO, &socket,
                                 sizeof(socket)) != sizeof(socket) ||
                socket.psi.soi_kind != SOCKINFO_UN)
                continue;
            const auto* bound = &socket.psi.soi_proto.pri_un.unsi_addr.ua_sun;
            if (QByteArray::fromRawData(bound->sun_path,
                                        static_cast<qsizetype>(::strnlen(
                                            bound->sun_path, sizeof(bound->sun_path)))) == wanted) {
                owners.push_back(pid);
                break;
            }
        }
    }
    return owners;
}
#endif

void terminate_all(const std::vector<qint64>& processes) {
    bool signaled = false;
    for (qint64 process : processes)
        signaled |= terminate(process);
    if (!signaled)
        return;
    QElapsedTimer elapsed;
    elapsed.start();
    while (elapsed.elapsed() < 3000) {
        if (std::all_of(processes.begin(), processes.end(), process_gone))
            return;
        pump(20);
    }
    for (qint64 process : processes)
        kill(process);
    pump(500);
}

class WorkspaceProbe {
  public:
    explicit WorkspaceProbe(QString manifest) : manifest_(std::move(manifest)) { open(); }

    ~WorkspaceProbe() {
        try {
            std::vector<QString> endpoints;
            if (workspace_)
                for (const auto& value : workspace_->sessions()) {
                    auto* session = value.value<SessionPreview*>();
                    if (session != nullptr)
                        if (const auto entry = session->reconnectEntry())
                            endpoints.push_back(entry->endpoint);
                }
            close();
#ifdef Q_OS_MACOS
            for (const auto& endpoint : endpoints)
                terminate_all(socket_owner_pids(endpoint));
#endif
            std::vector<qint64> services;
            services.reserve(observed_.size());
            for (const auto& record : observed_)
                services.push_back(record.child.service);
            terminate_all(services);
        } catch (const std::exception& error) {
            std::cerr << "Workspace fixture cleanup failed: " << error.what() << '\n';
        } catch (...) {
            std::cerr << "Workspace fixture cleanup failed\n";
        }
    }

    Workspace& workspace() { return *workspace_; }
    UiPreview& preview() { return *preview_; }
    QQuickWindow& window() { return *preview_->window(); }
    [[nodiscard]] const std::vector<SessionRecord>& observed() const { return observed_; }
    [[nodiscard]] QString layout_name() const { return keymap_.layoutName(); }

    void open() {
        QDir private_directory(QFileInfo(manifest_).absolutePath());
        require(private_directory.cdUp(), "Private temporary parent is missing");
        keymap_source_ = private_directory.filePath(QStringLiteral("appearance.json"));
        keymap_.setSourcePathForTesting(keymap_source_);
        keymap_.load();
        WorkspaceOptions options;
        options.manifest = manifest_;
        workspace_ = std::make_unique<Workspace>(WorkspaceMode::live, options);
        until([&] { return !workspace_->loading(); }, "Workspace manifest did not load");
        preview_ = std::make_unique<UiPreview>(
            *workspace_, UiPreviewOptions{.source = QUrl(QStringLiteral("qrc:/qml/Main.qml")),
                                          .compact = true,
                                          .screen = QString(),
                                          .keymap = &keymap_});
        require(preview_->load(), "Production QML did not load");
        window().requestActivate();
        lapis::desktop::test::activate_test_window(window());
        until([&] { return window().isActive(); }, "Production window did not activate");
    }

    void close() {
        preview_.reset();
        workspace_.reset();
        QThreadPool::globalInstance()->waitForDone();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }

    SessionPreview* add_shell(const QString& directory) {
        require(workspace().canAddSessions() && workspace().sessions().size() <= 1,
                "Probe workspace cannot create another shell");
        auto* dialog = window().findChild<QObject*>(QStringLiteral("sessionDialog"));
        require(dialog, "Session dialog is missing");
        click(window(), QStringLiteral("sessionTools"));
        auto* menu = window().findChild<QObject*>(QStringLiteral("sessionMenu"));
        require(menu, "Session menu is missing");
        wait_popup(*menu, true);
        click(window(), QStringLiteral("createSessionAction"));
        until([&] { return dialog->property("opened").toBool(); }, "Session dialog did not open");
        auto* directory_field =
            visual(window().contentItem(), QStringLiteral("sessionDirectoryField"));
        auto* endpoint_field =
            visual(window().contentItem(), QStringLiteral("sessionEndpointField"));
        require(directory_field && endpoint_field, "Session fields are missing");
        directory_field->setProperty("text", directory);
        endpoint_field->setProperty("text", QString());
        const auto count = workspace().sessions().size();
        click_text(window(), QStringLiteral("OK"));
        until([&] { return !dialog->property("visible").toBool(); },
              "Session dialog did not close after creation");
        until([&] { return workspace().sessions().size() > count; },
              "createSession did not add a shell");
        auto* session = workspace().sessions().constLast().value<SessionPreview*>();
        require(session && !session->sessionId().isEmpty(), "Added shell identity is invalid");
        until([&] { return session->inputReady(); }, "Created shell did not become ready");
        return session;
    }

    void record(SessionPreview& session, const QByteArray& marker) {
        const auto entry = session.reconnectEntry();
        if (!entry)
            throw std::runtime_error("Live session endpoint was not retained");
        observed_.push_back({session.sessionId(), process_probe(session, marker), entry->endpoint});
    }

    void close_session_at(int index) {
        require(index >= 0 && index < static_cast<int>(observed_.size()),
                "Observed session index is invalid");
        auto* session = workspace().session(observed_[static_cast<std::size_t>(index)].id);
        require(session, "Observed session disappeared");
        send(*session, QByteArrayLiteral("exit"));
        until([&] { return session->connectionState() == QStringLiteral("ended"); },
              "Shell exit did not end the session");
        until(
            [&] { return process_gone(observed_[static_cast<std::size_t>(index)].child.service); },
            "Detached service did not exit");
    }

  private:
    QString manifest_;
    QString keymap_source_;
    KeyMap keymap_;
    std::unique_ptr<Workspace> workspace_;
    std::unique_ptr<UiPreview> preview_;
    std::vector<SessionRecord> observed_;
};

QJsonArray process_array(const std::vector<SessionRecord>& records) {
    QJsonArray result;
    for (const auto& record : records)
        result.append(
            QJsonObject{{"shell_pid", record.child.shell}, {"service_pid", record.child.service}});
    return result;
}

void capture(WorkspaceProbe& probe, const QString& directory, const QString& name) {
    if (directory.isEmpty())
        return;
    require(QDir().mkpath(directory), "Could not create capture directory");
    pump(150);
    require(
        probe.window().grabWindow().save(QDir(directory).filePath(name + QStringLiteral(".png"))),
        "Workspace capture failed");
}
void exercise_layouts(WorkspaceProbe& probe, const QString& captures) {
    auto* dialog = probe.window().findChild<QObject*>(QStringLiteral("settingsDialog"));
    require(dialog, "Settings dialog is missing");
    for (const auto* layout : {"focus", "columns", "blocks", "stack"}) {
        require(probe.preview().openSettings(), "Settings dialog did not open");
        wait_popup(*dialog, true);
        click_setting(probe.window(), QStringLiteral("choice-") + QString::fromLatin1(layout));
        until([&] { return probe.layout_name() == QString::fromLatin1(layout); },
              "Layout choice was not applied");
        require(QMetaObject::invokeMethod(dialog, "close"), "Layout dialog did not close");
        wait_popup(*dialog, false);
        pump(100);
        require(visual(probe.window().contentItem(),
                       QStringLiteral("sessionCard_") + probe.observed().front().id) != nullptr,
                "Layout did not instantiate a real session card");
        capture(probe, captures, QString::fromLatin1(layout));
    }
    require(probe.layout_name() == QStringLiteral("stack"), "Layout sequence ended incorrectly");
}

struct ProbeOptions {
    QString json_path;
    QString image_path;
};

ProbeOptions parse_options(const QGuiApplication& app) {
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Opt-in macOS workspace GUI qualification"));
    parser.addHelpOption();
    const QCommandLineOption json_option(QStringList{"json-file"}, "Sanitized JSON report path.",
                                         QStringLiteral("path"));
    const QCommandLineOption image_option(
        QStringList{"output-dir"}, "Optional PNG capture directory.", QStringLiteral("directory"));
    parser.addOptions({json_option, image_option});
    parser.process(app);
    return {parser.value(json_option), parser.value(image_option)};
}

bool write_json(const QString& path, const QJsonObject& report) {
    if (path.isEmpty())
        return true;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
        return false;
    const QByteArray bytes = QJsonDocument(report).toJson(QJsonDocument::Indented);
    return file.write(bytes) == bytes.size() && file.flush();
}

int run_probe(const ProbeOptions& options) {
    QTemporaryDir directory(QStringLiteral("/private/tmp/lapis-workspace-ui-XXXXXX"));
    require(directory.isValid(), "Private temporary directory could not be created");
    const QString manifest =
        QFileInfo(directory.filePath(QStringLiteral("owner/workspace.json"))).absoluteFilePath();
    QJsonObject report{{"scope", "opt-in macOS production Workspace/QML qualification"},
                       {"platform", QSysInfo::productVersion()},
                       {"graphics_api", QStringLiteral("Vulkan")},
                       {"qml", QStringLiteral("qrc:/qml/Main.qml")},
                       {"shell", QStringLiteral("/bin/sh")}};
    std::vector<ChildProcesses> reopened;
    WorkspaceProbe probe(manifest);
    try {
        require(probe.workspace().sessions().isEmpty() && probe.workspace().canAddSessions(),
                "Initial production workspace was not empty");
        capture(probe, options.image_path, QStringLiteral("empty"));
        SessionPreview* first = probe.add_shell(directory.path());
        probe.record(*first, QByteArrayLiteral("GUI_A"));
        const auto first_size = first->snapshot().size;
        probe.workspace().setInteractionBlocked(QStringLiteral("held-key"), true);
        SessionPreview* second = probe.add_shell(directory.path());

        probe.workspace().setInteractionBlocked(QStringLiteral("held-key"), false);
        until([&] { return probe.workspace().focusedSession() == second; },
              "Deferred card focus did not switch");
        probe.record(*second, QByteArrayLiteral("GUI_B"));
        require(first->snapshot().size == first_size,
                "Background shell geometry changed during preview focus");
        require(probe.observed()[0].child != probe.observed()[1].child,
                "The two shells did not have independent process identities");
        const QString card_a = QStringLiteral("sessionCard_") + probe.observed()[0].id;
        const QString card_b = QStringLiteral("sessionCard_") + probe.observed()[1].id;
        click(probe.window(), card_a);
        until([&] { return probe.workspace().focusedSession() == first; },
              "Qt click did not switch to the first card");
        click(probe.window(), card_b);
        until([&] { return probe.workspace().focusedSession() == second; },
              "Qt click did not switch to the second card");
        // Foreground panes intentionally resize; only background geometry must stay put.
        const auto background_size = first->snapshot().size;
        probe.window().resize(1100, 760);
        pump(200);
        require(first->snapshot().size == background_size,
                "Window resize changed a background terminal");
        exercise_layouts(probe, options.image_path);
        report.insert("layouts", QJsonArray{"focus", "columns", "blocks", "stack"});
        report.insert("initial",
                      QJsonObject{{"processes", process_array(probe.observed())},
                                  {"background_geometry",
                                   QJsonObject{{"columns", static_cast<int>(first_size.columns)},
                                               {"rows", static_cast<int>(first_size.rows)}}}});
        if (!options.image_path.isEmpty()) {
            QDir().mkpath(options.image_path);
            const QString image = QFileInfo(options.image_path, QStringLiteral("workspace-ui.png"))
                                      .absoluteFilePath();
            require(probe.window().grabWindow().save(image), "Window capture failed");
            report.insert("capture", true);
        }

        probe.close();
        probe.open();
        auto* restored_first = probe.workspace().session(probe.observed()[0].id);
        auto* restored_second = probe.workspace().session(probe.observed()[1].id);
        require(restored_first && restored_second, "Restored workspace identities are missing");
        until([&] { return restored_first->inputReady() && restored_second->inputReady(); },
              "Restored shells did not become ready");
        for (int index = 0; index < 2; ++index) {
            auto* restored = index == 0 ? restored_first : restored_second;
            const QByteArray label =
                index == 0 ? QByteArrayLiteral("GUI_REOPEN_A") : QByteArrayLiteral("GUI_REOPEN_B");
            reopened.push_back(process_probe(*restored, label));
        }
        require(probe.observed()[0].id == restored_first->sessionId() &&
                    probe.observed()[1].id == restored_second->sessionId(),
                "Restored workspace IDs changed");
        require(reopened == std::vector<ChildProcesses>{probe.observed()[0].child,
                                                        probe.observed()[1].child},
                "Restored shell or service identities changed");
        report.insert("restored", QJsonObject{{"processes", process_array(probe.observed())},
                                              {"stable", true}});
        report.insert("status", QStringLiteral("passed"));
        probe.close_session_at(0);
        probe.close_session_at(1);
        report["status"] = QStringLiteral("passed-clean");
    } catch (const std::exception& error) {
        report.insert("status", QStringLiteral("failed"));
        report.insert("error", QString::fromUtf8(error.what()).left(2048));
        report.insert(
            "limitations",
            QStringLiteral("failure stopped qualification; owning probe cleans up on unwind"));
        if (!write_json(options.json_path, report))
            std::cerr << "Could not write JSON report\n";
        throw;
    }
    report.insert("limitations",
                  QStringLiteral("Qt event GUI delivery only; native OS input, GPU performance, "
                                 "and physical presentation latency are not measured"));
    require(write_json(options.json_path, report), "Could not write JSON report");
    return 0;
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication::setAttribute(Qt::AA_MacDontSwapCtrlAndMeta);
    qputenv("QT_MTL_NO_TRANSACTION", "1");
    qputenv("QT_VULKAN_LIB", LAPIS_VULKAN_LIBRARY);
    qputenv("SHELL", "/bin/sh");
    QGuiApplication application(argc, argv);
    application.setQuitOnLastWindowClosed(false);
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Vulkan);
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    qmlRegisterUncreatableType<SessionPreview>("Lapis", 1, 0, "SessionPreview",
                                               "Owned by workspace");
    qmlRegisterType<TerminalSurface>("Lapis", 1, 0, "TerminalSurface");
    try {
        return run_probe(parse_options(application));
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
