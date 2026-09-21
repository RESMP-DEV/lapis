#include "ui_capture.hpp"
#include "terminal_surface.hpp"
#include "ui_preview.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QQuickWindow>
#include <QTimer>
#include <algorithm>
#include <stdexcept>
#include <utility>
#include <vector>

namespace lapis::desktop {
namespace {
QQuickItem* visual_item(QQuickItem& parent, const QString& name) {
    if (parent.objectName() == name)
        return &parent;
    for (auto* child : parent.childItems())
        if (auto* match = visual_item(*child, name))
            return match;
    return nullptr;
}
QJsonObject view_state(QQuickWindow& window, Workspace& workspace) {
    QJsonArray cards;
    for (const auto& entry : workspace.sessions()) {
        auto* session = entry.value<SessionPreview*>();
        auto* item = visual_item(*window.contentItem(),
                                 QStringLiteral("sessionCard_") + session->sessionId());
        QJsonObject card{{"id", session->sessionId()},
                         {"pending", session->attentionPending()},
                         {"serial", static_cast<qint64>(session->attentionSerial())},
                         {"requests", session->attentionCount()}};
        if (item) {
            card.insert("width", item->width());
            card.insert("height", item->height());
            card.insert("x", item->x());
            card.insert("y", item->y());
            card.insert("cue_running", item->property("cueRunning").toBool());
            card.insert("cue_level", item->property("cueLevel").toDouble());
        }
        cards.append(card);
    }
    auto* focused = window.findChild<QQuickItem*>(QStringLiteral("focusedPane"));
    const auto size = workspace.focusedSession()->snapshot().size;
    auto* terminal = visual_item(*window.contentItem(), QStringLiteral("liveTerminal"));
    // Blocks mode hides the single pane and promotes the focused tile instead,
    // so either surface owning focus means the session owns the keyboard.
    auto* tile = visual_item(*window.contentItem(), QStringLiteral("cardTerminal_") +
                                                        workspace.focusedSession()->sessionId());
    const bool owns_focus = (terminal && terminal->hasFocus()) || (tile && tile->hasFocus());
    return {
        {"terminal_owns_focus", owns_focus},
        {"window_active", window.isActive()},
        {"focus", window.activeFocusItem() ? window.activeFocusItem()->objectName() : QString{}},
        {"cards", cards},
        {"terminal_columns", size.columns},
        {"terminal_rows", size.rows},
        {"pane_height", focused ? focused->height() : 0},
        {"pane_width", focused ? focused->width() : 0}};
}
void type_smoke_text(QQuickWindow& window, const QString& text) {
    for (const QChar character : text) {
        QKeyEvent event(QEvent::KeyPress, character.toUpper().unicode(), Qt::NoModifier,
                        QString(character));
        QCoreApplication::sendEvent(&window, &event);
    }
}
bool send_smoke_input(QQuickWindow& window, UiPreview& preview) {
    if (!preview.assignTerminalFocus())
        return false;
    auto* terminal = qobject_cast<TerminalSurface*>(window.activeFocusItem());
    if (!terminal || !terminal->isVisible() || !terminal->isEnabled() || !terminal->interactive())
        return false;
    qInfo() << "Shell smoke input owns ready terminal" << terminal->objectName();
    QKeyEvent discard(QEvent::KeyPress, Qt::Key_X, Qt::NoModifier, QStringLiteral("discard-this"));
    QCoreApplication::sendEvent(&window, &discard);
    QKeyEvent clear_line(QEvent::KeyPress, Qt::Key_U, Qt::ControlModifier, QStringLiteral("u"));
    QCoreApplication::sendEvent(&window, &clear_line);
    type_smoke_text(window, QStringLiteral("printf '\\nLAPIS_INPUT_%s_OK\\n' WRONG"));
    // Exercise real shell Meta word motion/deletion, including macOS Option text.
    QKeyEvent back_word(QEvent::KeyPress, Qt::Key_B, Qt::AltModifier, QString::fromUtf8("∫"));
    QCoreApplication::sendEvent(&window, &back_word);
    QKeyEvent delete_word(QEvent::KeyPress, Qt::Key_D, Qt::AltModifier, QString::fromUtf8("∂"));
    QCoreApplication::sendEvent(&window, &delete_word);
    type_smoke_text(window,
                    QStringLiteral("%1; stty size").arg(QCoreApplication::applicationPid()));
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier, QStringLiteral("\r"));
    QCoreApplication::sendEvent(&window, &enter);
    return true;
}
class Capture final : public QObject {
  public:
    Capture(QQuickWindow& window, Workspace& workspace, UiPreview& preview, CaptureOptions options)
        : QObject(&window), window_(window), workspace_(workspace), preview_(preview),
          options_(std::move(options)) {
        setObjectName(QStringLiteral("lapisWindowCapture"));
        connect(&preview_, &UiPreview::windowChanged, this, [this](QQuickWindow* current) {
            if (current != &window_) {
                timeout_.stop();
                disconnect(&window_, nullptr, this, nullptr);
                deleteLater();
            }
        });
        clock_.start();
        frames_.reserve(512);
        connect(
            &window_, &QQuickWindow::frameSwapped, this, [this] { frame(); }, Qt::QueuedConnection);
        // A retained scene can finish its first frame before macOS activates the
        // window. Request a frame on activation so capture can observe readiness.
        connect(&window_, &QWindow::activeChanged, this, [this] { window_.update(); });
        timeout_.setSingleShot(true);
        timeout_.setInterval(15000 + options_.delay_ms);
        connect(&timeout_, &QTimer::timeout, this, [this] {
            qCritical() << "Window capture timed out; active:" << window_.isActive()
                        << "input ready:" << workspace_.focusedSession()->inputReady()
                        << "smoke sent:" << sent_ << "focus:"
                        << (window_.activeFocusItem() ? window_.activeFocusItem()->objectName()
                                                      : QString{});
            QCoreApplication::exit(1);
        });
        timeout_.start();
    }

  private:
    void frame() {
        if (preview_.window() != &window_)
            return;
        if (frames_.size() < 4096)
            frames_.push_back(static_cast<double>(clock_.nsecsElapsed()) / 1000000.0);
        if (started_)
            return;
        if (!window_.isActive())
            return;
        if (!workspace_.previewMode() && !workspace_.focusedSession()->liveSnapshotReady())
            return;
        try {
            if (options_.smoke_input && !sent_) {
                // Screen restoration precedes asynchronous identity persistence.
                // Do not discard the one-shot command while input is still gated.
                if (workspace_.focusedSession()->inputReady())
                    sent_ = send_smoke_input(window_, preview_);
                return;
            }
            const auto marker = QStringLiteral("LAPIS_INPUT_%1_OK")
                                    .arg(QCoreApplication::applicationPid())
                                    .toStdU32String();
            if (options_.smoke_input && workspace_.focusedSession()->snapshot().graphemes.find(
                                            marker) == std::u32string::npos)
                return;
            started_ = true;
            before_ = view_state(window_, workspace_);
            scenario_started_ms_ = static_cast<double>(clock_.nsecsElapsed()) / 1000000.0;
            if (options_.scenario != QStringLiteral("none"))
                static_cast<void>(workspace_.replayAttention(options_.scenario));
            QTimer::singleShot(options_.delay_ms, this, [this] {
                try {
                    finish();
                } catch (const std::exception& error) {
                    qCritical().noquote() << error.what();
                    QCoreApplication::exit(1);
                }
            });
        } catch (const std::exception& error) {
            qCritical().noquote() << error.what();
            QCoreApplication::exit(1);
        }
    }
    bool save_trace() {
        if (options_.trace_path.isEmpty())
            return true;
        QJsonArray times;
        std::vector<double> active_intervals;
        int idle_frames = 0;
        for (std::size_t i = 0; i < frames_.size(); ++i) {
            times.append(frames_[i]);
            if (frames_[i] - scenario_started_ms_ >= 1500.0)
                ++idle_frames;
            if (i && frames_[i] >= scenario_started_ms_ &&
                frames_[i] - scenario_started_ms_ < 1500.0)
                active_intervals.push_back(frames_[i] - frames_[i - 1]);
        }
        std::ranges::sort(active_intervals);
        const auto percentile = [&active_intervals](double fraction) {
            if (active_intervals.empty())
                return 0.0;
            const auto index = static_cast<std::size_t>(
                fraction * static_cast<double>(active_intervals.size() - 1));
            return active_intervals[index];
        };
        const QJsonObject trace{
            {"scope", "GUI-thread observations of frameSwapped; includes delivery scheduling, not "
                      "physical presentation or input latency"},
            {"scenario", options_.scenario},
            {"preview", workspace_.previewMode()},
            {"reduced_motion", preview_.reducedMotion()},
            {"system_reduced_motion", preview_.systemReducedMotion()},
            {"width", window_.width()},
            {"height", window_.height()},
            {"before", before_},
            {"after", view_state(window_, workspace_)},
            {"frame_times_ms", times},
            {"scenario_started_ms", scenario_started_ms_},
            {"interval_p50_ms", percentile(0.50)},
            {"interval_p95_ms", percentile(0.95)},
            {"interval_p99_ms", percentile(0.99)},
            {"frames_after_1500ms", idle_frames},
            {"observed_ms", static_cast<double>(clock_.nsecsElapsed()) / 1000000.0}};
        QFile file(options_.trace_path);
        const auto bytes = QJsonDocument(trace).toJson();
        return file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
               file.write(bytes) == bytes.size();
    }
    void finish() {
        if (preview_.window() != &window_)
            return;
        timeout_.stop();
        // Record before grabWindow(), which itself can request an additional frame.
        const bool traced = save_trace();
        const auto capture = window_.grabWindow();
        const bool saved = !capture.isNull() && capture.save(options_.image_path);
        qInfo() << "Window capture" << (saved ? "saved" : "failed") << options_.image_path;
        if (options_.smoke_input)
            qInfo("Qt input to PTY to snapshot: PASS");
        if (!traced)
            qCritical() << "Could not save frame trace" << options_.trace_path;
        QCoreApplication::exit(saved && traced ? 0 : 1);
    }
    QQuickWindow& window_;
    Workspace& workspace_;
    UiPreview& preview_;
    CaptureOptions options_;
    QTimer timeout_;
    QElapsedTimer clock_;
    std::vector<double> frames_;
    QJsonObject before_;
    double scenario_started_ms_{};
    bool started_{};
    bool sent_{};
};
} // namespace
void capture_window(QQuickWindow& window, Workspace& workspace, UiPreview& preview,
                    CaptureOptions options) {
    // QObject window parent owns this one-shot observer and cancels its timers on destruction.
    if (window.findChild<QObject*>(QStringLiteral("lapisWindowCapture"),
                                   Qt::FindDirectChildrenOnly) != nullptr)
        return;
    new Capture(window, workspace, preview, std::move(options));
}
} // namespace lapis::desktop
