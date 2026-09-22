#include "clipboard_backup.hpp"
#include "platform/window_activation.hpp"
#include "terminal_surface.hpp"
#include "ui_preview.hpp"
#include "workspace_supervisor.hpp"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QInputMethodEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QKeySequence>
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
#include <QTimer>
#include <mach/mach.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <numeric>
#include <signal.h>
#include <source_location>
#include <stdexcept>
#include <string>
#include <sys/resource.h>
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

void require(bool condition, const std::string& message,
             std::source_location where = std::source_location::current()) {
    require(condition, message.c_str(), where);
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

void send_key(QQuickItem& item, int key, bool press) {
    QKeyEvent event(press ? QEvent::KeyPress : QEvent::KeyRelease, key, Qt::NoModifier);
    item.forceActiveFocus();
    require(item.window(), "Key fixture has no window");
    QCoreApplication::sendEvent(item.window(), &event);
}

void send_drag(QQuickWindow& window, bool enter) {
    // The exact platform MIME payload is irrelevant here: the production host
    // reacts to the real top-level drag event type itself.
    QEvent event(enter ? QEvent::DragEnter : QEvent::DragLeave);
    event.setAccepted(false);
    QCoreApplication::sendEvent(&window, &event);
}

void send_item_preedit(QQuickItem& item, const QString& text) {
    QList<QInputMethodEvent::Attribute> attributes;
    QInputMethodEvent event(text, attributes);
    QCoreApplication::sendEvent(&item, &event);
}

void send_paste(QQuickItem& item) {
    const auto combination = QKeySequence(QKeySequence::Paste)[0];
    QKeyEvent press(QEvent::KeyPress, combination.key(), combination.keyboardModifiers(),
                    QStringLiteral("v"));
    QKeyEvent release(QEvent::KeyRelease, combination.key(), combination.keyboardModifiers(),
                      QString());
    QCoreApplication::sendEvent(&item, &press);
    QCoreApplication::sendEvent(&item, &release);
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
    explicit WorkspaceProbe(QString manifest, std::function<qint64()> supervisor_clock = {})
        : manifest_(std::move(manifest)), supervisor_clock_(std::move(supervisor_clock)) {
        open();
    }

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
                                          .keymap = &keymap_,
                                          .supervisor_clock = supervisor_clock_});
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
    std::function<qint64()> supervisor_clock_;
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

qint64 memory_bytes() {
    struct rusage usage{};
    return ::getrusage(RUSAGE_SELF, &usage) == 0 ? static_cast<qint64>(usage.ru_maxrss)
                                                 : qint64{-1};
}

QJsonObject summarize(QVector<qint64> values) {
    if (values.isEmpty())
        return {};
    std::sort(values.begin(), values.end());
    const auto percentile = [&values](double fraction) {
        const int index = std::clamp(
            static_cast<int>(std::round(static_cast<double>(values.size() - 1) * fraction)), 0,
            static_cast<int>(values.size() - 1));
        return values.at(index);
    };
    return QJsonObject{
        {"samples", values.size()},
        {"minimum", values.first()},
        {"p50", percentile(0.50)},
        {"p95", percentile(0.95)},
        {"p99", percentile(0.99)},
        {"maximum", values.last()},
        {"mean", static_cast<double>(std::accumulate(values.begin(), values.end(), qint64{0})) /
                     static_cast<double>(values.size())}};
}

qint64 resident_bytes() {
    mach_task_basic_info_data_t info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    require(task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info),
                      &count) == KERN_SUCCESS,
            "Current GUI resident memory sample failed");
    return static_cast<qint64>(info.resident_size);
}

QJsonObject timing_metadata(const QQuickWindow& window) {
    return QJsonObject{
        {"endpoint", QStringLiteral("QQuickWindow::frameSwapped callback on the GUI thread")},
        {"physical_latency", false},
        {"clock", QStringLiteral("QElapsedTimer::nsecsElapsed() monotonic nanoseconds")},
        {"display_refresh_hz", window.screen() ? window.screen()->refreshRate() : -1},
        {"request", QStringLiteral("Workspace::setFocusedIndex or SessionPreview::sendText")},
        {"units", QStringLiteral("nanoseconds")},
        {"session_count", 2},
        {"workload", QStringLiteral("30 alternating warm switches and 30 controlled shell printf "
                                    "commands; no continuous background output")}};
}

struct ProbeOptions {
    QString json_path;
    QString image_path;
};

QJsonObject guarded_carousel_acceptance(WorkspaceProbe& probe, qint64& injected_now) {
    auto& workspace = probe.workspace();
    auto* supervisor = probe.preview().supervisor();
    auto* first = workspace.session(probe.observed().at(0).id);
    auto* second = workspace.session(probe.observed().at(1).id);
    require(first && second && supervisor, "Carousel fixture lost a production object");

    QJsonArray cases;
    click(probe.window(), QStringLiteral("workspaceRequests"));
    auto* controls = probe.window().findChild<QObject*>(QStringLiteral("workspaceAttentionQueue"));
    require(controls, "Workspace controls are missing");
    wait_popup(*controls, true);
    click(probe.window(), QStringLiteral("carouselEnabled"));
    require(supervisor->enabled(), "Automatic checkbox did not enable the supervisor");
    click(probe.window(), QStringLiteral("carouselPaused"));
    require(supervisor->paused(), "Pause checkbox did not pause the supervisor");
    click(probe.window(), QStringLiteral("carouselPaused"));
    require(!supervisor->paused(), "Pause checkbox did not resume the supervisor");
    click(probe.window(), QStringLiteral("carouselPinned"));
    require(supervisor->pinned(), "Pin checkbox did not pin the current session");
    click(probe.window(), QStringLiteral("carouselPinned"));
    require(!supervisor->pinned(), "Pin checkbox did not unpin the current session");
    click(probe.window(), QStringLiteral("carouselEnabled"));
    require(!supervisor->enabled(), "Automatic checkbox did not disable the supervisor");
    click(probe.window(), QStringLiteral("workspaceQueueClose"));
    wait_popup(*controls, false);
    until(
        [&] {
            return probe.window().isActive() && !workspace.interactionBlocked() &&
                   first->inputReady() && second->inputReady();
        },
        "Carousel controls did not return to an eligible live window");
    pump(20);
    const auto current_id = [&] { return workspace.focusedSession()->sessionId(); };
    const auto current_index = [&] { return workspace.focusedIndex(); };
    const auto tick_blocked = [&](const QString& name, bool require_block) {
        const QString expected = current_id();
        if (require_block)
            require(
                workspace.interactionBlocked(),
                QString(name + QStringLiteral(" did not publish a Workspace block")).toStdString());
        supervisor->tick();
        require(current_id() == expected,
                QString(name + QStringLiteral(" allowed an automatic switch")).toStdString());
        cases.append(QJsonObject{{"case", name},
                                 {"focused_before", expected},
                                 {"focused_after", current_id()},
                                 {"interaction_blocked", workspace.interactionBlocked()},
                                 {"switched", false},
                                 {"passed", true}});
    };
    const auto release_and_switch = [&](const QString& name) {
        const QString expected = current_id();
        const QString destination =
            first->sessionId() == expected ? second->sessionId() : first->sessionId();
        injected_now += 5001;
        supervisor->tick();
        require(current_id() == destination,
                QString(name + QStringLiteral(" did not resume automatic switching after release"))
                    .toStdString());
        cases.append(QJsonObject{{"case", name},
                                 {"focused_before", expected},
                                 {"focused_after", destination},
                                 {"switched", true},
                                 {"passed", true}});
    };

    const auto initial_focus = current_id();
    supervisor->setEnabled(true);
    supervisor->tick();
    require(current_id() == initial_focus, "Enabling the carousel skipped its initial dwell");
    cases.append(QJsonObject{{"case", QStringLiteral("enabled respects initial dwell")},
                             {"switched", false},
                             {"passed", true}});

    injected_now += 5001;
    const QString quiet_before = current_id();
    supervisor->tick();
    require(current_id() != quiet_before,
            (QStringLiteral("Eligible supervisor did not visit the quiet session: ") +
             supervisor->status() +
             QStringLiteral(" active=%1 blocked=%2 first=%3 second=%4")
                 .arg(probe.window().isActive())
                 .arg(workspace.interactionBlocked())
                 .arg(first->inputReady())
                 .arg(second->inputReady()))
                .toStdString());
    cases.append(QJsonObject{{"case", QStringLiteral("eligible quiet session switch")},
                             {"switched", true},
                             {"passed", true}});

    supervisor->setPaused(true);
    injected_now += 5001;
    tick_blocked(QStringLiteral("paused blocks automatic switch"), false);
    supervisor->setPaused(false);
    release_and_switch(QStringLiteral("paused resumes after explicit unpause"));

    supervisor->setPinned(true);
    injected_now += 5001;
    tick_blocked(QStringLiteral("pin blocks automatic departure"), false);
    workspace.setFocusedIndex(current_index() == 0 ? 1 : 0);
    supervisor->setPinned(false);
    cases.append(QJsonObject{{"case", QStringLiteral("pin preserves manual navigation")},
                             {"switched", true},
                             {"passed", true}});

    injected_now += 5001;
    workspace.setFocusedIndex(workspace.focusedIndex());
    injected_now += 2999;
    tick_blocked(QStringLiteral("manual cooldown blocks before 3000 ms"), false);
    injected_now += 2;
    release_and_switch(QStringLiteral("manual cooldown releases after 3000 ms"));

    auto* terminal =
        visual(probe.window().contentItem(), QStringLiteral("cardTerminal_") + current_id());
    require(terminal, "Focused terminal is missing for typing guard");
    send_key(*terminal, Qt::Key_A, true);
    send_key(*terminal, Qt::Key_A, false);
    injected_now += 1000;
    tick_blocked(QStringLiteral("recent typing blocks before 1500 ms"), false);
    release_and_switch(QStringLiteral("typing releases after quiet interval"));

    terminal = visual(probe.window().contentItem(), QStringLiteral("cardTerminal_") + current_id());
    require(terminal, "Focused terminal is missing for held-key guard");
    send_key(*terminal, Qt::Key_A, true);
    require(workspace.interactionBlocked(), "Held root key did not publish a Workspace block");
    injected_now += 5001;
    tick_blocked(QStringLiteral("held key blocks after input quiet interval"), true);
    terminal = visual(probe.window().contentItem(), QStringLiteral("cardTerminal_") + current_id());
    require(terminal, "Focused terminal is missing to release held key");
    send_key(*terminal, Qt::Key_A, false);
    release_and_switch(QStringLiteral("held key releases automatic switching"));

    send_drag(probe.window(), true);
    require(workspace.interactionBlocked(), "Mouse drag did not publish a Workspace block");
    injected_now += 5001;
    tick_blocked(QStringLiteral("mouse drag blocks after input quiet interval"), true);
    send_drag(probe.window(), false);
    release_and_switch(QStringLiteral("mouse drag releases automatic switching"));

    const QString focused_card = QStringLiteral("cardTerminal_") + current_id();
    terminal = visual(probe.window().contentItem(), focused_card);
    require(terminal, "Focused terminal is missing for composition and paste guards");
    send_item_preedit(*terminal, QStringLiteral("composition"));
    require(workspace.interactionBlocked(), "IME composition did not publish a Workspace block");
    injected_now += 5001;
    tick_blocked(QStringLiteral("IME composition blocks after input quiet interval"), true);
    send_item_preedit(*terminal, QString());
    release_and_switch(QStringLiteral("IME composition releases automatic switching"));

    terminal = visual(probe.window().contentItem(), QStringLiteral("cardTerminal_") + current_id());
    require(terminal, "Focused terminal is missing for paste guard");
    bool paste_block_observed = false;
    bool paste_owner_preserved = false;
    QObject paste_observation;
    const auto paste_connection =
        QObject::connect(&workspace, &Workspace::interactionChanged, &paste_observation, [&] {
            const QString paste_source = current_id();
            if (!workspace.interactionBlocked() || paste_block_observed)
                return;
            paste_block_observed = true;
            injected_now += 5001;
            supervisor->tick();
            paste_owner_preserved = current_id() == paste_source;
        });
    {
        lapis::desktop::test::ClipboardBackup clipboard;
        QGuiApplication::clipboard()->setText(QStringLiteral("workspace-paste"));
        send_paste(*terminal);
    }
    QObject::disconnect(paste_connection);
    require(paste_block_observed, "Paste block was not observed while paste was active");
    require(paste_owner_preserved, "Automatic switch split a paste across sessions");
    cases.append(QJsonObject{{"case", QStringLiteral("paste block prevents synchronous switch")},
                             {"interaction_blocked_during_paste", true},
                             {"switched", false},
                             {"passed", true}});
    release_and_switch(QStringLiteral("paste releases automatic switching"));

    auto* attention_queue =
        probe.window().findChild<QObject*>(QStringLiteral("workspaceAttentionQueue"));
    require(attention_queue, "Workspace attention queue is missing");
    require(QMetaObject::invokeMethod(attention_queue, "open"), "Workspace queue did not open");
    wait_popup(*attention_queue, true);
    until([&] { return workspace.interactionBlocked(); },
          "Modal workspace queue did not publish a Workspace block");
    injected_now += 5001;
    tick_blocked(QStringLiteral("modal workspace queue blocks automatic switching"), true);
    require(QMetaObject::invokeMethod(attention_queue, "close"), "Workspace queue did not close");
    wait_popup(*attention_queue, false);
    until([&] { return !workspace.interactionBlocked(); }, "Modal block did not clear");
    release_and_switch(QStringLiteral("modal releases automatic switching"));

    QQuickWindow inactive_window;
    inactive_window.setTitle(QStringLiteral("lapis workspace inactive qualification"));
    inactive_window.setGeometry(140, 140, 480, 320);
    inactive_window.show();
    inactive_window.requestActivate();
    lapis::desktop::test::activate_test_window(inactive_window);
    until([&] { return inactive_window.isActive() && !probe.window().isActive(); },
          "Real second window did not take OS activation");
    injected_now += 5001;
    tick_blocked(QStringLiteral("inactive window blocks automatic switching"), false);
    probe.window().requestActivate();
    lapis::desktop::test::activate_test_window(probe.window());
    until([&] { return probe.window().isActive() && !inactive_window.isActive(); },
          "Production window did not regain OS activation");
    release_and_switch(QStringLiteral("reactivated window resumes automatic switching"));

    return {{"cases", cases},
            {"automatic_default_enabled", false},
            {"production_controls_exercised", true},
            {"injected_clock", true},
            {"session_count", 2},
            {"real_window_activation", true}};
}

QJsonObject guarded_attention_acceptance(WorkspaceProbe& probe, qint64& injected_now) {
    auto& workspace = probe.workspace();
    auto* supervisor = probe.preview().supervisor();
    auto* first = workspace.session(probe.observed().at(0).id);
    auto* second = workspace.session(probe.observed().at(1).id);
    require(first && second && supervisor, "Attention fixture lost a production object");

    supervisor->setEnabled(false);
    workspace.setFocusedIndex(0);
    until([&] { return workspace.focusedSession() == first; },
          "Guard fixture focus did not settle");
    const QString survivor_id = first->sessionId();
    const QString obsolete_destination = QStringLiteral("workspace/no-such-session");
    supervisor->tick();
    require(workspace.focusedSession()->sessionId() == survivor_id,
            "Supervisor lost its live focused identity");
    require(!workspace.focusAutomatically(obsolete_destination),
            "Workspace accepted an obsolete automatic destination");
    require(workspace.focusedSession()->sessionId() == survivor_id,
            "Failed automatic focus changed the live destination");

    supervisor->setEnabled(true);
    first->olderHistory();
    until([&] { return first->historyActive() && !first->historyRequestPending(); },
          "History fixture did not enter read-only mode");
    injected_now += 5001;
    supervisor->tick();
    require(workspace.focusedSession()->sessionId() == survivor_id,
            "Supervisor switched away from read-only history");
    first->returnToLive();
    until([&] { return first->inputReady(); }, "History fixture did not return to live input");

    supervisor->setPaused(true);
    first->reconnect();
    until([&] { return first->inputReady(); }, "Supervisor disconnect fixture did not reconnect");
    supervisor->tick();
    require(workspace.focusedSession()->sessionId() == survivor_id,
            "Reconnect discarded the surviving session identity");
    require(supervisor->pendingCount() == 0, "Reconnect replayed resolved source state");
    supervisor->setEnabled(false);
    supervisor->setPaused(false);

    return {{"obsolete_queue_not_replayed", true},
            {"history_read_only_guarded", true},
            {"disconnect_reconciled_live_state", true},
            {"obsolete_switch_rejected", true}};
}

QJsonObject two_session_timing_baseline(WorkspaceProbe& probe) {
    constexpr int sample_count = 30;
    auto& workspace = probe.workspace();
    auto* supervisor = probe.preview().supervisor();
    supervisor->setEnabled(false);
    auto* first = workspace.session(probe.observed().at(0).id);
    auto* second = workspace.session(probe.observed().at(1).id);
    require(first && second, "Timing fixture lost a two-session source");

    quint64 focused_frame = 0;
    quint64 marker_frame = 0;
    bool timing_switch_armed = false;
    bool switch_observed = false;
    bool marker_updated = false;
    SessionPreview* frame_destination = nullptr;
    QString frame_marker;
    QObject observation;
    workspace.setFocusedIndex(0);
    until([&] { return workspace.focusedSession() == first; },
          "Timing initial focus did not settle");
    pump(20);
    const auto timing_focus_connection =
        QObject::connect(&workspace, &Workspace::focusChanged, &observation, [&] {
            if (timing_switch_armed && workspace.focusedSession() == frame_destination)
                switch_observed = true;
        });
    const auto frame_connection =
        QObject::connect(&probe.window(), &QQuickWindow::frameSwapped, &observation, [&] {
            if (frame_destination != nullptr && workspace.focusedSession() == frame_destination &&
                switch_observed)
                ++focused_frame;
            if (frame_destination && !frame_marker.isEmpty() && marker_updated &&
                screen(*frame_destination).startsWith(frame_marker))
                ++marker_frame;
        });
    const auto observeSnapshot = [&](SessionPreview* source) {
        if (frame_destination == source && !frame_marker.isEmpty() &&
            screen(*source).startsWith(frame_marker))
            marker_updated = true;
    };
    QObject::connect(first, &SessionPreview::snapshotChanged, &observation,
                     [&] { observeSnapshot(first); });
    QObject::connect(second, &SessionPreview::snapshotChanged, &observation,
                     [&] { observeSnapshot(second); });
    const auto initial_resident_bytes = resident_bytes();
    QVector<qint64> switch_times;
    QVector<qint64> input_times;
    for (int sample = 0; sample < sample_count; ++sample) {
        auto* destination = sample % 2 == 0 ? second : first;
        const quint64 initial_focused_frame = focused_frame;
        frame_destination = destination;
        switch_observed = false;
        timing_switch_armed = true;
        QElapsedTimer timer;
        timer.start();
        workspace.setFocusedIndex(sample % 2 == 0 ? 1 : 0);
        until(
            [&] {
                return workspace.focusedSession() == destination &&
                       focused_frame > initial_focused_frame;
            },
            "Warm switch did not render the newly focused session");
        switch_times.append(timer.nsecsElapsed());
        timing_switch_armed = false;

        const QString marker = QStringLiteral("TIMING_%1").arg(sample, 2, 10, QLatin1Char('0'));
        frame_marker = marker;
        const quint64 initial_marker_frame = marker_frame;
        marker_updated = false;
        timer.start();
        destination->sendText(QByteArray(1, '\x15'));
        destination->sendText(QByteArray("printf '\\033[2J\\033[H%s\\n' '") + marker.toUtf8() +
                              "'\r");
        until(
            [&] {
                return marker_updated && screen(*destination).startsWith(marker) &&
                       marker_frame > initial_marker_frame;
            },
            "Controlled terminal output did not reach a displayed frame");
        input_times.append(timer.nsecsElapsed());
        QCoreApplication::processEvents(QEventLoop::AllEvents, 1);
        frame_marker.clear();
    }
    QObject::disconnect(frame_connection);
    QObject::disconnect(timing_focus_connection);
    supervisor->setEnabled(false);

    struct rusage before{};
    require(::getrusage(RUSAGE_SELF, &before) == 0, "Idle CPU baseline failed");
    int idle_frames = 0;
    QEventLoop idle;
    QObject::connect(&probe.window(), &QQuickWindow::frameSwapped, &idle, [&] { ++idle_frames; });
    QTimer::singleShot(1000, &idle, &QEventLoop::quit);
    const auto idle_start = std::chrono::steady_clock::now();
    idle.exec();
    struct rusage usage{};
    require(::getrusage(RUSAGE_SELF, &usage) == 0, "Idle CPU sample failed");
    const auto wall_microseconds = std::chrono::duration_cast<std::chrono::microseconds>(
                                       std::chrono::steady_clock::now() - idle_start)
                                       .count();
    const auto cpu_before = (before.ru_utime.tv_sec + before.ru_stime.tv_sec) * 1000000LL +
                            (before.ru_utime.tv_usec + before.ru_stime.tv_usec);
    const auto cpu_total = (usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) * 1000000LL +
                           (usage.ru_utime.tv_usec + usage.ru_stime.tv_usec);
    const auto cpu_microseconds = cpu_total - cpu_before;

    return {{"warm_switch_request_to_frame_swapped", summarize(switch_times)},
            {"terminal_input_to_frame_swapped", summarize(input_times)},
            {"metadata", timing_metadata(probe.window())},
            {"memory", QJsonObject{{"ru_maxrss_bytes", memory_bytes()},
                                   {"resident_before_samples_bytes", initial_resident_bytes},
                                   {"resident_after_samples_bytes", resident_bytes()},
                                   {"measurement", QStringLiteral("own process high-water RSS")},
                                   {"scope", QStringLiteral("GUI process only")}}},
            {"idle_activity",
             QJsonObject{{"observation_window_us", wall_microseconds},
                         {"frame_swapped_callbacks", idle_frames},
                         {"cpu_microseconds", cpu_microseconds},
                         {"cpu_percent", wall_microseconds > 0
                                             ? static_cast<double>(cpu_microseconds) /
                                                   static_cast<double>(wall_microseconds) * 100.0
                                             : -1},
                         {"cpu_thread_capacity", QThread::idealThreadCount()},
                         {"method",
                          QStringLiteral(
                              "own-process getrusage delta over one second; not GPU activity")}}}};
}

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
    qint64 injected_now = 1000000;
    WorkspaceProbe probe(manifest, [&injected_now] { return injected_now; });
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
        report.insert("guarded_carousel", guarded_carousel_acceptance(probe, injected_now));
        report.insert("timing_baseline", two_session_timing_baseline(probe));
        report.insert("guarded_attention", guarded_attention_acceptance(probe, injected_now));
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

        probe.preview().supervisor()->setEnabled(true);
        probe.preview().supervisor()->setPaused(true);
        probe.preview().supervisor()->setPinned(true);
        probe.close();
        probe.open();
        require(!probe.preview().supervisor()->enabled() &&
                    !probe.preview().supervisor()->paused() &&
                    !probe.preview().supervisor()->pinned(),
                "Reopening restored automatic navigation state");
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
                                              {"stable", true},
                                              {"carousel_reset_to_off", true}});
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
                  QStringLiteral("Qt event delivery exercises root wiring; native physical input "
                                 "and photon latency are not measured by this probe"));
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
