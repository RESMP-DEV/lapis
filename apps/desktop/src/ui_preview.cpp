#include "ui_preview.hpp"
#include "app_paths.hpp"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QKeySequence>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlError>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRect>
#include <QSaveFile>
#include <QScopeGuard>
#include <QScopedValueRollback>
#include <QScreen>
#include <QStringList>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <functional>
#include <utility>

namespace lapis::desktop {
namespace {

constexpr int kMaximumDiagnosticsLength = 4096;

[[nodiscard]] QString formatDiagnostics(const QList<QQmlError>& errors) {
    QStringList messages;
    messages.reserve(errors.size());
    for (const QQmlError& error : errors)
        messages.append(error.toString());
    return messages.join(QLatin1Char('\n'));
}

[[nodiscard]] QString boundedDiagnostics(const QString& diagnostics) {
    if (diagnostics.size() <= kMaximumDiagnosticsLength)
        return diagnostics;
    return diagnostics.first(kMaximumDiagnosticsLength - 3) + QStringLiteral("...");
}

[[nodiscard]] QString appendDiagnostics(const QString& current, const QString& addition) {
    if (addition.isEmpty())
        return boundedDiagnostics(current);
    if (current.isEmpty())
        return boundedDiagnostics(addition);
    return boundedDiagnostics(current + QLatin1Char('\n') + addition);
}

[[nodiscard]] bool isLocalSource(const QUrl& source) {
    return source.isValid() && source.isLocalFile();
}

// Move a window onto the requested screen and keep it inside that screen's
// usable area. Returns false when no screen name matches so the caller can
// surface that fallback placement is being used.
[[nodiscard]] bool move_to_screen(QQuickWindow& window, const QString& requested) {
    const auto screens = QGuiApplication::screens();
    QScreen* target = nullptr;
    for (QScreen* screen : screens) {
        if (screen->name().contains(requested, Qt::CaseInsensitive)) {
            target = screen;
            break;
        }
    }
    if (target == nullptr) {
        QStringList names;
        names.reserve(screens.size());
        for (QScreen* screen : screens)
            names.append(screen->name());
        qWarning().noquote() << "No screen matched" << requested
                             << "; available:" << names.join(QStringLiteral(", "));
        return false;
    }
    const QRect available = target->availableGeometry();
    const QSize minimum = window.minimumSize();
    if (minimum.width() > available.width() || minimum.height() > available.height()) {
        qWarning().noquote() << "Requested screen available area is smaller than window minimum:"
                             << available.size() << "<" << minimum;
    }
    // Honor the QML minimum while fitting the window to the panel. If that
    // minimum exceeds the panel, the resulting overflow is logged above.
    QRect geometry = window.geometry();
    if (geometry.width() > available.width())
        geometry.setWidth(qMax(available.width(), minimum.width()));
    if (geometry.height() > available.height())
        geometry.setHeight(qMax(available.height(), minimum.height()));
    geometry.moveTopLeft(available.topLeft());
    window.setScreen(target);
    window.setGeometry(geometry);
    const QRect placed = window.geometry();
    qInfo().noquote() << "lapis window screen:" << target->name() << "at" << placed.x()
                      << placed.y() << "size" << placed.width() << placed.height();
    return true;
}

// Settings are local runtime state, never shared appearance configuration.
// Refuse symlinks, foreign-owned objects, and permissive existing files. A
// current-user directory left permissive by project tooling is repaired only
// when saving new geometry.
[[nodiscard]] bool privateGeometryPath(const QString& path, bool create) {
    const QString directory = QFileInfo(path).absolutePath();
    if (create && !QFileInfo::exists(directory)) {
        const QFileInfo directoryInfo(directory);
        const QFileInfo parentInfo(directoryInfo.absolutePath());
        // Only create the final settings directory. Recursive creation can
        // follow a dangling or intermediate symlink before validation.
        if (!parentInfo.isDir() || parentInfo.isSymbolicLink() ||
            !QDir(parentInfo.absoluteFilePath())
                 .mkdir(directoryInfo.fileName(),
                        QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner))
            return false;
    }
    const QByteArray directoryBytes = QFile::encodeName(directory);
    // Pin the directory inode before deciding whether it may be repaired. This
    // rejects a symlink directly and keeps lstat/chmod from acting on a path
    // that is renamed while permissions are being inspected.
    const int directory_fd =
        ::open(directoryBytes.constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (directory_fd < 0)
        return false;
    const auto close_directory = qScopeGuard([directory_fd] { ::close(directory_fd); });
    struct stat info{};
    if (::fstat(directory_fd, &info) != 0 || !S_ISDIR(info.st_mode) || info.st_uid != ::getuid())
        return false;
    if ((info.st_mode & 0077U) != 0) {
        // A project tool can legitimately create runtime/ with the umask's
        // default mode. Repair only the save path and only this pinned, real,
        // current-user directory; readers never mutate the checkout.
        if (!create || ::fchmod(directory_fd, 0700) != 0)
            return false;
        if (::fstat(directory_fd, &info) != 0 || (info.st_mode & 0077U) != 0)
            return false;
    }
    const struct stat pinned_directory = info;
    if (::lstat(directoryBytes.constData(), &info) != 0 || info.st_dev != pinned_directory.st_dev ||
        info.st_ino != pinned_directory.st_ino || (info.st_mode & 0077U) != 0)
        return false;
    const QByteArray pathBytes = QFile::encodeName(path);
    if (::lstat(pathBytes.constData(), &info) != 0)
        return errno == ENOENT;
    return S_ISREG(info.st_mode) && info.st_uid == ::getuid() && (info.st_mode & 0077U) == 0;
}

[[nodiscard]] QRect visibleGeometry(QRect geometry, const QSize& minimum) {
    QScreen* target = QGuiApplication::primaryScreen();
    for (QScreen* screen : QGuiApplication::screens()) {
        if (screen->availableGeometry().contains(geometry.center())) {
            target = screen;
            break;
        }
    }
    if (target == nullptr)
        return {};
    const QRect available = target->availableGeometry();
    geometry.setSize(geometry.size().expandedTo(minimum).boundedTo(available.size()));
    geometry.moveLeft(
        qBound(available.left(), geometry.left(), available.right() - geometry.width() + 1));
    geometry.moveTop(
        qBound(available.top(), geometry.top(), available.bottom() - geometry.height() + 1));
    return geometry;
}

} // namespace

UiPreview::UiPreview(Workspace& workspace, UiPreviewOptions options, QObject* parent)
    : QObject(parent), workspace_(workspace), options_(std::move(options)) {
    refreshSettingsShortcuts();
    if (options_.keymap != nullptr)
        connect(options_.keymap, &KeyMap::changed, this, [this] {
            refreshSettingsShortcuts();
            deferTerminalFocus();
        });
    connect(&workspace_, &Workspace::focusChanged, this, &UiPreview::deferTerminalFocus);
}

QStringList UiPreview::monospaceFamilies() const {
    QStringList families;
    for (const QString& family : QFontDatabase::families()) {
        // Private platform faces are not user-selectable.
        if (!QFontDatabase::isPrivateFamily(family) && QFontDatabase::isFixedPitch(family))
            families.append(family);
    }
    families.sort(Qt::CaseInsensitive);
    return families;
}

void UiPreview::refreshSettingsShortcuts() {
    settings_shortcuts_ = options_.keymap != nullptr
                              ? options_.keymap->sequences(QStringLiteral("openSettings"))
                              : default_settings_shortcuts();
    parsed_settings_shortcuts_.clear();
    for (const auto& text : settings_shortcuts_) {
        const QKeySequence sequence(text);
        parsed_settings_shortcuts_.append(sequence);
#ifndef Q_OS_MACOS
        if (sequence.count() == 1)
            if (const auto shifted = shifted_punctuation(sequence[0]))
                parsed_settings_shortcuts_.append(QKeySequence(*shifted));
#endif
    }
    emit settingsShortcutsChanged();
}

UiPreview::~UiPreview() {
    shutting_down_ = true;
    // A close event already saved the last visible placement. After native
    // teardown Qt can report the old platform geometry, so do not overwrite
    // that record when destroying an already closed window.
    if (window_ && window_->isVisible())
        saveGeometry();
    if (window_) {
        // Qt 6.11 queues render-thread signal proxies whose targets must still
        // exist when delivered. Release first, then deliver before deleting QML.
        window_->setPersistentGraphics(false);
        window_->setPersistentSceneGraph(false);
        window_->hide();
        window_->releaseResources();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    }
    engine_.reset();
    const auto retired =
        findChildren<QQmlApplicationEngine*>(QString{}, Qt::FindDirectChildrenOnly);
    for (auto* engine : retired)
        delete engine;
}

QQuickWindow* UiPreview::window() const { return window_.data(); }

void UiPreview::setReducedMotion(bool enabled) {
    if (reduced_motion_ == enabled)
        return;

    const bool wasReduced = reducedMotion();
    reduced_motion_ = enabled;
    if (reducedMotion() != wasReduced)
        emit reducedMotionChanged();
}

void UiPreview::setSystemReducedMotion(bool enabled) {
    if (system_reduced_motion_ == enabled)
        return;

    system_reduced_motion_ = enabled;
    emit reducedMotionChanged();
}

bool UiPreview::load() {
    if (shutting_down_)
        return false;
    if (engine_) {
        qWarning() << "UiPreview load rejected: initial load has already completed";
        return false;
    }

    return loadCandidate();
}

bool UiPreview::reload() {
    if (shutting_down_)
        return false;
    if (!active()) {
        qWarning() << "UiPreview reload rejected: preview mode is not active";
        return false;
    }

    if (!isLocalSource(options_.source)) {
        const QString message =
            QStringLiteral("UiPreview reload rejected: source is not a local file URL: %1")
                .arg(options_.source.toString());
        qWarning().noquote() << message;
        return false;
    }

    return loadCandidate();
}

bool UiPreview::assignTerminalFocus() {
    if (shutting_down_)
        return false;
    QQuickWindow* target_window = window();
    if (target_window == nullptr)
        return false;
    if (target_window->property("inputBlocked").toBool())
        return false;
    const QString name = QStringLiteral("liveTerminal");
    QQuickItem* terminal = nullptr;
    const std::function<void(QQuickItem&)> visit = [&](QQuickItem& item) {
        if (terminal != nullptr)
            return;
        if (item.objectName() == name) {
            terminal = &item;
            return;
        }
        for (QQuickItem* child : item.childItems())
            visit(*child);
    };
    visit(*target_window->contentItem());
    if (terminal == nullptr || !terminal->isVisible() || !terminal->isEnabled())
        return false;
    terminal->forceActiveFocus(Qt::OtherFocusReason);
    return terminal->hasActiveFocus();
}

void UiPreview::deferTerminalFocus() {
    if (shutting_down_)
        return;
    QMetaObject::invokeMethod(
        this,
        [this] {
            QQuickWindow* target_window = window();
            if (target_window != nullptr && target_window->isActive())
                assignTerminalFocus();
        },
        Qt::QueuedConnection);
}

bool UiPreview::eventFilter(QObject* watched, QEvent* event) {
    if (watched == window_.data() && event->type() == QEvent::Close)
        saveGeometry();
    if (event->type() != QEvent::KeyPress)
        return QObject::eventFilter(watched, event);

    auto* current_window = qobject_cast<QQuickWindow*>(watched);
    if (current_window == nullptr || current_window != window_.data() ||
        !current_window->isActive())
        return QObject::eventFilter(watched, event);

    auto* key_event = static_cast<QKeyEvent*>(event);
    if (key_event->isAutoRepeat())
        return false;
    for (const auto& sequence : parsed_settings_shortcuts_) {
        if (sequence.count() == 1 && sequence[0] == key_event->keyCombination() && openSettings()) {
            event->accept();
            return true;
        }
    }
    return false;
}

bool UiPreview::openSettings() {
    if (shutting_down_)
        return false;
    QQuickWindow* target_window = window();
    if (target_window == nullptr)
        return false;
    if (target_window->property("inputBlocked").toBool())
        return false;
    const auto* terminal = target_window->findChild<QQuickItem*>(QStringLiteral("liveTerminal"));
    if (terminal != nullptr &&
        (terminal->property("composing").toBool() || terminal->property("pasting").toBool()))
        return false;
    // Invoke the dialog through QML rather than duplicating its state in C++.
    // The function lives on the root Window, which is the QML root object; the
    // content item is a child and does not carry it.
    if (engine_ == nullptr || engine_->rootObjects().isEmpty())
        return false;
    QObject* root = engine_->rootObjects().first();
    return root != nullptr &&
           QMetaObject::invokeMethod(root, "openSettingsDialog", Qt::DirectConnection);
}

bool UiPreview::geometryPersistenceEnabled() const {
    return options_.persistGeometry && !active() && options_.screen.isEmpty();
}

void UiPreview::restoreGeometry(QQuickWindow& target) {
    if (options_.geometryPath.isEmpty())
        options_.geometryPath =
            QDir(data_directory()).filePath(QStringLiteral("runtime/window.json"));
    if (!privateGeometryPath(options_.geometryPath, false))
        return;
    QFile file(options_.geometryPath);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 4096)
        return;
    const auto object = QJsonDocument::fromJson(file.readAll()).object();
    if (object.value(QStringLiteral("version")).toInt() != 1)
        return;
    const int x = object.value(QStringLiteral("x")).toInt();
    const int y = object.value(QStringLiteral("y")).toInt();
    const int width = object.value(QStringLiteral("width")).toInt();
    const int height = object.value(QStringLiteral("height")).toInt();
    if (width < 1 || height < 1 || width > 32768 || height > 32768 ||
        qAbs(static_cast<qint64>(x)) > 32768 || qAbs(static_cast<qint64>(y)) > 32768)
        return;
    QRect geometry(x, y, width, height);
    geometry = visibleGeometry(geometry, target.minimumSize());
    if (!geometry.isValid())
        return;
    target.setGeometry(geometry);
    normal_geometry_ = geometry;
    if (object.value(QStringLiteral("maximized")).toBool())
        target.setWindowState(Qt::WindowMaximized);
}

void UiPreview::saveGeometry() {
    if (!geometryPersistenceEnabled() || window_ == nullptr)
        return;
    // Native configure notifications can lag a requested move or resize. The
    // current normal window geometry is authoritative when closing; the cache
    // is only needed while maximized/minimized, whose geometry is not the
    // user's normal placement.
    if (window_->windowState() == Qt::WindowNoState)
        normal_geometry_ = window_->geometry();
    if (!normal_geometry_.isValid())
        return;
    if (options_.geometryPath.isEmpty())
        options_.geometryPath =
            QDir(data_directory()).filePath(QStringLiteral("runtime/window.json"));
    if (!privateGeometryPath(options_.geometryPath, true)) {
        qWarning() << "Window geometry not saved: runtime path must be private and owned by you";
        return;
    }
    const QJsonObject object{
        {QStringLiteral("version"), 1},
        {QStringLiteral("x"), normal_geometry_.x()},
        {QStringLiteral("y"), normal_geometry_.y()},
        {QStringLiteral("width"), normal_geometry_.width()},
        {QStringLiteral("height"), normal_geometry_.height()},
        {QStringLiteral("maximized"), window_->windowState() == Qt::WindowMaximized}};
    QSaveFile file(options_.geometryPath);
    const QByteArray bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
    if (!file.open(QIODevice::WriteOnly) ||
        !file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner) ||
        file.write(bytes) != bytes.size() || !file.commit())
        qWarning() << "Could not save window geometry:" << file.errorString();
}

void UiPreview::rememberGeometry() {
    if (window_ && window_->windowState() == Qt::WindowNoState)
        normal_geometry_ = window_->geometry();
}

void UiPreview::configureGeometry(QQuickWindow& target, bool reloading) {
    if (!reloading && geometryPersistenceEnabled())
        restoreGeometry(target);
    connect(&target, &QWindow::xChanged, this, &UiPreview::rememberGeometry);
    connect(&target, &QWindow::yChanged, this, &UiPreview::rememberGeometry);
    connect(&target, &QWindow::widthChanged, this, &UiPreview::rememberGeometry);
    connect(&target, &QWindow::heightChanged, this, &UiPreview::rememberGeometry);
    rememberGeometry();
}

void UiPreview::publishWarnings(const QList<QQmlError>& warnings) {
    for (const QQmlError& warning : warnings)
        qWarning().noquote() << warning.toString();
    if (publishing_diagnostics_)
        return;
    const auto next = appendDiagnostics(diagnostics_, formatDiagnostics(warnings));
    if (next == diagnostics_)
        return;
    const QScopedValueRollback guard(publishing_diagnostics_, true);
    diagnostics_ = next;
    emit diagnosticsChanged();
}

bool UiPreview::loadCandidate() {
    std::unique_ptr<QQmlApplicationEngine> candidate = std::make_unique<QQmlApplicationEngine>();
    candidate->rootContext()->setContextProperty(QStringLiteral("workspace"), &workspace_);
    candidate->rootContext()->setContextProperty(QStringLiteral("preview"), this);
    // QML reads `keymap.actionSequences(...)`. Absent keymap keeps the literals.
    if (options_.keymap != nullptr)
        candidate->rootContext()->setContextProperty(QStringLiteral("keymap"), options_.keymap);
    if (options_.alerts)
        candidate->rootContext()->setContextProperty(QStringLiteral("alerts"), options_.alerts);
    if (options_.agentSearch)
        candidate->rootContext()->setContextProperty(QStringLiteral("agentSearch"),
                                                     options_.agentSearch);
    if (options_.usage)
        candidate->rootContext()->setContextProperty(QStringLiteral("usage"), options_.usage);
    candidate->setInitialProperties({{QStringLiteral("visible"), false}});

    QString candidateDiagnostics;
    const QMetaObject::Connection loadDiagnosticsConnection =
        QObject::connect(candidate.get(), &QQmlApplicationEngine::warnings, candidate.get(),
                         [&candidateDiagnostics](const QList<QQmlError>& warnings) {
                             for (const QQmlError& warning : warnings)
                                 qWarning().noquote() << warning.toString();
                             candidateDiagnostics = appendDiagnostics(candidateDiagnostics,
                                                                      formatDiagnostics(warnings));
                         });

    const bool reloading = engine_ != nullptr;
    const QRect previousGeometry = reloading && window_ ? window_->geometry() : QRect();

    candidate->load(options_.source);
    QObject::disconnect(loadDiagnosticsConnection);

    const auto setDiagnostics = [this](const QString& diagnostics) {
        const QString bounded = boundedDiagnostics(diagnostics);
        if (diagnostics_ == bounded)
            return;
        diagnostics_ = bounded;
        emit diagnosticsChanged();
    };

    const QList<QObject*> roots = candidate->rootObjects();
    if (roots.isEmpty()) {
        if (candidateDiagnostics.isEmpty())
            candidateDiagnostics = QStringLiteral("QML load produced no root object");
        setDiagnostics(candidateDiagnostics);
        qWarning().noquote() << diagnostics_;
        return false;
    }

    QQuickWindow* candidateWindow = qobject_cast<QQuickWindow*>(roots.front());
    if (candidateWindow == nullptr) {
        const QString message =
            QStringLiteral("QML root object is not a QQuickWindow: %1")
                .arg(QString::fromLatin1(roots.front()->metaObject()->className()));
        qWarning().noquote() << message;
        if (candidateDiagnostics.isEmpty())
            candidateDiagnostics = message;
        setDiagnostics(candidateDiagnostics);
        qWarning().noquote() << diagnostics_;
        return false;
    }

    if (reloading)
        candidateWindow->setGeometry(previousGeometry);
    else if (options_.compact)
        candidateWindow->resize(980, 700);

    candidateWindow->hide();

    QObject::connect(
        candidateWindow, &QQuickWindow::sceneGraphInitialized, candidateWindow,
        [candidateWindow] {
            qInfo() << "lapis scene graph API:"
                    << candidateWindow->rendererInterface()->graphicsApi();
            if (candidateWindow->rendererInterface()->graphicsApi() != QSGRendererInterface::Vulkan)
                qFatal("Requested Vulkan renderer was not selected");
        },
        Qt::DirectConnection);
    QObject::connect(candidate.get(), &QQmlApplicationEngine::warnings, this,
                     &UiPreview::publishWarnings);

    const QPointer<QQuickWindow> acceptedWindow = candidateWindow;
    setDiagnostics(candidateDiagnostics);

    std::swap(engine_, candidate);
    window_ = acceptedWindow;
    if (reloading) {
        // A QML method in this engine may still be on the call stack, including
        // a second reload in that same method. Retire only through the event loop.
        candidate->setParent(this);
        candidate.release()->deleteLater();
    }

    emit windowChanged(candidateWindow);
    candidateWindow->installEventFilter(this);
    if (!options_.screen.isEmpty() && !reloading &&
        !move_to_screen(*candidateWindow, options_.screen)) {
        setDiagnostics(
            appendDiagnostics(candidateDiagnostics,
                              QStringLiteral("No screen matched '%1'; see log for available names")
                                  .arg(options_.screen)));
    }
    configureGeometry(*candidateWindow, reloading);
    candidateWindow->show();

    return true;
}

} // namespace lapis::desktop
