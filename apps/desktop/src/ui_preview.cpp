#include "ui_preview.hpp"

#include <QDebug>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlError>
#include <QQuickWindow>
#include <QRect>

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

} // namespace

UiPreview::UiPreview(Workspace& workspace, UiPreviewOptions options, QObject* parent)
    : QObject(parent), workspace_(workspace), options_(std::move(options)) {}

UiPreview::~UiPreview() {
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

bool UiPreview::loadCandidate() {
    std::unique_ptr<QQmlApplicationEngine> candidate = std::make_unique<QQmlApplicationEngine>();
    candidate->rootContext()->setContextProperty(QStringLiteral("workspace"), &workspace_);
    candidate->rootContext()->setContextProperty(QStringLiteral("preview"), this);
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
    if (diagnostics_ != candidateDiagnostics) {
        diagnostics_ = boundedDiagnostics(candidateDiagnostics);
        emit diagnosticsChanged();
    }

    std::swap(engine_, candidate);
    window_ = acceptedWindow;
    if (reloading) {
        // A QML method in this engine may still be on the call stack, including
        // a second reload in that same method. Retire only through the event loop.
        candidate->setParent(this);
        candidate.release()->deleteLater();
    }

    emit windowChanged(candidateWindow);
    candidateWindow->show();

    return true;
}

} // namespace lapis::desktop
