#include "keymap.hpp"
#include "terminal_surface.hpp"
#include "ui_preview.hpp"
#include "workspace.hpp"

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QPoint>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QRect>
#include <QSGRendererInterface>
#include <QScopeGuard>
#include <QScreen>
#include <QSize>
#include <QTemporaryDir>
#include <QThread>
#include <QUrl>

#include <functional>
#include <iostream>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace {
using namespace lapis::desktop;

void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}

void await(const std::function<bool()>& predicate, const char* message) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < 3000) {
        QCoreApplication::processEvents();
        QThread::msleep(2);
    }
    require(predicate(), message);
}

QByteArray read(const QString& path) {
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "read geometry fixture");
    return file.readAll();
}

void write(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    require(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "write geometry fixture");
    require(file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner),
            "private geometry fixture");
    require(file.write(bytes) == bytes.size(), "complete fixture write");
}

UiPreviewOptions options(const QString& path) {
    return {.source = QUrl::fromLocalFile(QStringLiteral(LAPIS_QML_SOURCE)),
            .screen = {},
            .persistGeometry = true,
            .geometryPath = path};
}

void show(UiPreview& view) {
    require(view.load(), "load actual workspace QML");
    require(view.window() != nullptr, "workspace has a native window");
    await([&] { return view.window()->isExposed(); }, "workspace window exposed");
}

void save_normal_window(Workspace& workspace, const QString& path) {
    UiPreview view(workspace, options(path));
    show(view);
    require(view.window()->close(), "close workspace without terminating its sessions");
}

void remembered_geometry_and_offscreen_restore(Workspace& workspace, const QTemporaryDir& temp) {
    const QString path = temp.filePath(QStringLiteral("geometry.json"));
    QRect saved;
    QByteArray closed_registry;
    {
        UiPreview view(workspace, options(path));
        show(view);
        const QRect available = view.window()->screen()->availableGeometry();
        const QRect desired(available.topLeft() + QPoint(70, 70), QSize(720, 520));
        view.window()->setGeometry(desired);
        await([&] { return view.window()->geometry() == desired; }, "window moved and resized");
        saved = view.window()->geometry();
        require(view.window()->close(), "close saves the resized geometry");
        closed_registry = read(path);
    }
    require(read(path) == closed_registry, "destruction cannot overwrite the saved close geometry");
    require(QFileInfo::exists(path), "normal workspace writes geometry");
    struct stat info{};
    require(::stat(QFile::encodeName(path).constData(), &info) == 0 && (info.st_mode & 0077U) == 0,
            "saved geometry is private");
    {
        UiPreview restored(workspace, options(path));
        show(restored);
        try {
            await([&] { return restored.window()->geometry() == saved; },
                  "normal geometry survives window destruction and recreation");
        } catch (const std::runtime_error&) {
            const auto actual = restored.window()->geometry();
            const auto frame = restored.window()->frameGeometry();
            std::cerr << "geometry restore expected=" << saved.x() << ',' << saved.y() << ' '
                      << saved.width() << 'x' << saved.height() << " actual=" << actual.x() << ','
                      << actual.y() << ' ' << actual.width() << 'x' << actual.height()
                      << " frame=" << frame.x() << ',' << frame.y() << ' ' << frame.width() << 'x'
                      << frame.height() << " file=" << read(path).constData() << '\n';
            throw;
        }
        require(restored.window()->close(), "close restored geometry");
    }
    write(path, R"({"version":1,"x":30000,"y":30000,"width":800,"height":600})");
    {
        UiPreview restored(workspace, options(path));
        show(restored);
        const QRect available = restored.window()->screen()->availableGeometry();
        require(available.contains(restored.window()->geometry()),
                "offscreen saved geometry fits the current screen work area");
        require(restored.window()->close(), "close clamped geometry");
    }
}

void isolated_modes_leave_geometry_untouched(Workspace& workspace, const QTemporaryDir& temp) {
    const QString path = temp.filePath(QStringLiteral("untouched.json"));
    const QByteArray sentinel = R"({"version":1,"x":111,"y":112,"width":700,"height":510})";
    write(path, sentinel);
    {
        Workspace fixture(WorkspaceMode::preview);
        UiPreview preview(fixture, options(path));
        show(preview);
        require(preview.window()->close(), "close fixture preview");
    }
    require(read(path) == sentinel, "preview cannot overwrite user window geometry");
    {
        auto settings = options(path);
        settings.screen = QGuiApplication::primaryScreen()->name();
        require(!settings.screen.isEmpty(), "isolated display has a screen name");
        UiPreview placed(workspace, settings);
        show(placed);
        require(placed.window()->geometry().topLeft() != QPoint(111, 112),
                "explicit screen placement ignores saved position");
        require(placed.window()->close(), "close explicitly placed window");
    }
    require(read(path) == sentinel, "explicit screen override cannot overwrite user geometry");
    {
        auto settings = options(path);
        settings.persistGeometry = false;
        UiPreview probe(workspace, settings);
        show(probe);
        require(probe.window()->close(), "close persistence-disabled probe");
    }
    require(read(path) == sentinel, "opt-out cannot overwrite user geometry");
}

void unsafe_paths_are_preserved(Workspace& workspace, const QTemporaryDir& temp) {
    const QByteArray sentinel = "do not replace this file";
    const QString target = temp.filePath(QStringLiteral("target.json"));
    const QString link = temp.filePath(QStringLiteral("linked.json"));
    write(target, sentinel);
    require(QFile::link(target, link), "create geometry file symlink");
    save_normal_window(workspace, link);
    require(QFileInfo(link).isSymbolicLink() && read(target) == sentinel,
            "geometry save preserves a symlink and its target");

    // Project scripts commonly create runtime/ with the process umask. Saving
    // may tighten only that current-user-owned real directory; a permissive
    // existing geometry file still stays untouched.
    const QString directory = temp.filePath(QStringLiteral("runtime"));
    require(QDir().mkdir(directory), "create default-mode runtime directory");
    require(QFile::setPermissions(directory, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                                 QFileDevice::ExeOwner | QFileDevice::ReadGroup |
                                                 QFileDevice::ExeGroup | QFileDevice::ReadOther |
                                                 QFileDevice::ExeOther),
            "make a current-user directory with default read and traverse bits");
    const QString geometry_path = QDir(directory).filePath(QStringLiteral("window.json"));
    save_normal_window(workspace, geometry_path);
    struct stat repaired_directory{};
    struct stat repaired_file{};
    require(::lstat(QFile::encodeName(directory).constData(), &repaired_directory) == 0 &&
                S_ISDIR(repaired_directory.st_mode) && repaired_directory.st_uid == ::getuid() &&
                (repaired_directory.st_mode & 0077U) == 0,
            "saving repairs a current-user runtime directory to 0700");
    require(::lstat(QFile::encodeName(geometry_path).constData(), &repaired_file) == 0 &&
                S_ISREG(repaired_file.st_mode) && repaired_file.st_uid == ::getuid() &&
                (repaired_file.st_mode & 0077U) == 0,
            "the repaired directory receives private geometry");

    require(QFile::setPermissions(target, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                              QFileDevice::ReadGroup),
            "make nonprivate geometry file");
    save_normal_window(workspace, target);
    require(read(target) == sentinel, "reject nonprivate existing file without modifying it");

    const QString directory_link = temp.filePath(QStringLiteral("directory-link"));
    require(QFile::link(temp.path(), directory_link), "create directory symlink");
    const QString escaped_path = QDir(directory_link).filePath(QStringLiteral("escaped.json"));
    save_normal_window(workspace, escaped_path);
    require(!QFileInfo::exists(temp.filePath(QStringLiteral("escaped.json"))),
            "directory symlink must not redirect geometry writes");
}

void legacy_layout_still_focuses_single_stage(const QTemporaryDir& temp) {
    Workspace fixture(WorkspaceMode::preview);
    KeyMap keymap;
    keymap.setSourcePathForTesting(temp.filePath(QStringLiteral("legacy-appearance.json")));
    require(keymap.setLayout(QStringLiteral("blocks")), "legacy blocks appearance loads");
    auto settings = options(temp.filePath(QStringLiteral("legacy-window.json")));
    settings.keymap = &keymap;
    UiPreview view(fixture, settings);
    show(view);
    view.window()->requestActivate();
    await([&] { return view.window()->isActive(); }, "isolated workspace gets native focus");
    require(view.assignTerminalFocus(), "legacy appearance still focuses the single stage");
    require(view.window()->activeFocusItem() != nullptr &&
                view.window()->activeFocusItem()->objectName() == QStringLiteral("liveTerminal"),
            "keyboard ownership belongs to the sole terminal stage, never a legacy card");
    require(view.window()->close(), "close single-stage focus fixture");
}

void modal_focus_remains_owned(const QTemporaryDir& temp) {
    Workspace workspace(WorkspaceMode::preview);
    KeyMap keymap;
    keymap.setSourcePathForTesting(temp.filePath(QStringLiteral("appearance.json")));
    require(keymap.setLayout(QStringLiteral("blocks")), "legacy layout fixture");
    auto settings = options(temp.filePath(QStringLiteral("focus.json")));
    settings.keymap = &keymap;
    settings.persistGeometry = false;
    UiPreview view(workspace, settings);
    show(view);
    require(view.assignTerminalFocus(), "visible stage accepts focus before modal");
    require(view.openSettings(), "settings opens over the visible stage");
    await([&] { return view.window()->property("inputBlocked").toBool(); }, "settings owns input");
    require(!view.assignTerminalFocus(), "deferred terminal focus cannot steal a modal's input");
    require(!view.openSettings(), "settings cannot be reentered through C++ interception");
    require(view.window()->close(), "close workspace safely with legacy layout configured");
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication::setAttribute(Qt::AA_MacDontSwapCtrlAndMeta);
    QGuiApplication application(argc, argv);
    application.setQuitOnLastWindowClosed(false);
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Vulkan);
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    qmlRegisterUncreatableType<SessionPreview>("Lapis", 1, 0, "SessionPreview",
                                               "Owned by workspace");
    qmlRegisterType<TerminalSurface>("Lapis", 1, 0, "TerminalSurface");
    // Render-thread shutdown posts signal proxies to the GUI thread. Finish
    // their delivery and deferred deletion before destroying the application.
    const auto drain = qScopeGuard([] {
        QCoreApplication::sendPostedEvents();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    });
    try {
        QTemporaryDir temp;
        require(temp.isValid(), "private window-state fixture directory");
        Workspace workspace(WorkspaceMode::live,
                            {.endpoint = {},
                             .launch = {},
                             .storagePath = temp.filePath(QStringLiteral("workspace.json"))});
        require(workspace.sessions().isEmpty(), "live fixture starts no agents or shell services");
        remembered_geometry_and_offscreen_restore(workspace, temp);
        isolated_modes_leave_geometry_untouched(workspace, temp);
        unsafe_paths_are_preserved(workspace, temp);
        legacy_layout_still_focuses_single_stage(temp);
        modal_focus_remains_owned(temp);
        std::cout << "window_state_test: all cases passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "window_state_test: " << error.what() << '\n';
        return 1;
    }
}
