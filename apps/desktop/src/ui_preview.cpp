#include "ui_preview.hpp"

#include <QDebug>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QKeySequence>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlError>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRect>
#include <QScreen>
#include <QSet>
#include <QStringList>

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
    connect(this, &UiPreview::heldKeysChanged, this, [this] {
        workspace_.setInteractionBlocked(QStringLiteral("root-held-keys"), holdingKeys());
    });
}

void UiPreview::refreshSettingsShortcuts() {
    settings_shortcuts_ = options_.keymap != nullptr
                              ? options_.keymap->sequences(QStringLiteral("openSettings"))
                              : default_settings_shortcuts();
    parsed_settings_shortcuts_.clear();
    for (const auto& text : settings_shortcuts_)
        parsed_settings_shortcuts_.append(QKeySequence(text));
    emit settingsShortcutsChanged();
}

UiPreview::~UiPreview() {
    if (window_ != nullptr)
        window_->removeEventFilter(this);
    clearHeldKeys();
    workspace_.setInteractionBlocked(QStringLiteral("root-modal"), false);
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
    if (engine_) {
        qWarning() << "UiPreview load rejected: initial load has already completed";
        return false;
    }

    return loadCandidate();
}

bool UiPreview::reload() {
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
    QQuickWindow* target_window = window();
    if (target_window == nullptr)
        return false;
    if (target_window->property("inputBlocked").toBool())
        return false;
    // Focus and columns keep the single live pane; blocks and stack promote one
    // tile per session. The dynamic workspace can be empty while loading.
    const KeyMap* keymap = options_.keymap;
    const bool pane_visible = keymap == nullptr || keymap->layout() == WorkspaceLayout::Focus ||
                              keymap->layout() == WorkspaceLayout::Columns;
    const SessionPreview* focused = workspace_.focusedSession();
    if (!pane_visible && focused == nullptr)
        return false;
    const QString name = pane_visible ? QStringLiteral("liveTerminal")
                                      : QStringLiteral("cardTerminal_") + focused->sessionId();
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
    auto* current_window = qobject_cast<QQuickWindow*>(watched);
    if (current_window == nullptr || current_window != window_.data())
        return QObject::eventFilter(watched, event);

    if (event->type() == QEvent::WindowDeactivate || event->type() == QEvent::Destroy)
        clearHeldKeys();

    if (event->type() != QEvent::KeyPress && event->type() != QEvent::KeyRelease)
        return QObject::eventFilter(watched, event);

    if (!current_window->isActive())
        return QObject::eventFilter(watched, event);

    auto* key_event = static_cast<QKeyEvent*>(event);
    updateHeldKey(*key_event, event->type() == QEvent::KeyPress);
    if (event->type() != QEvent::KeyPress || key_event->isAutoRepeat())
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
    QQuickWindow* target_window = window();
    if (target_window == nullptr)
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

void UiPreview::updateHeldKey(const QKeyEvent& event, bool pressed) {
    if (event.isAutoRepeat())
        return;
    const bool was_holding = holdingKeys();
    if (pressed)
        held_keys_.insert(event.key());
    else if (!held_keys_.remove(event.key()))
        return;
    if (was_holding != holdingKeys())
        emit heldKeysChanged();
}

void UiPreview::clearHeldKeys() {
    if (held_keys_.isEmpty())
        return;
    held_keys_.clear();
    emit heldKeysChanged();
}

bool UiPreview::loadCandidate() {
    std::unique_ptr<QQmlApplicationEngine> candidate = std::make_unique<QQmlApplicationEngine>();
    candidate->rootContext()->setContextProperty(QStringLiteral("workspace"), &workspace_);
    candidate->rootContext()->setContextProperty(QStringLiteral("preview"), this);
    // QML reads `keymap.actionSequences(...)`. Absent keymap keeps the literals.
    if (options_.keymap != nullptr)
        candidate->rootContext()->setContextProperty(QStringLiteral("keymap"), options_.keymap);
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
                     [this](const QList<QQmlError>& warnings) {
                         for (const QQmlError& warning : warnings)
                             qWarning().noquote() << warning.toString();
                         diagnostics_ =
                             appendDiagnostics(diagnostics_, formatDiagnostics(warnings));
                         emit diagnosticsChanged();
                     });

    const QPointer<QQuickWindow> acceptedWindow = candidateWindow;
    setDiagnostics(candidateDiagnostics);

    clearHeldKeys();
    workspace_.setInteractionBlocked(QStringLiteral("root-modal"),
                                     candidateWindow->property("inputBlocked").toBool());
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
    candidateWindow->show();

    return true;
}

} // namespace lapis::desktop
