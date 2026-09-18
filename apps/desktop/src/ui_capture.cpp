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
    return {
        {"terminal_owns_focus", terminal && terminal->hasFocus()},
        {"window_active", window.isActive()},
        {"focus", window.activeFocusItem() ? window.activeFocusItem()->objectName() : QString{}},
        {"cards", cards},
        {"terminal_columns", size.columns},
        {"terminal_rows", size.rows},
        {"pane_height", focused ? focused->height() : 0},
        {"pane_width", focused ? focused->width() : 0}};
}
void send_smoke_input(QQuickWindow& window) {
    auto* terminal = window.findChild<TerminalSurface*>(QStringLiteral("liveTerminal"));
    if (!terminal)
        throw std::runtime_error("Live terminal surface missing");
    terminal->forceActiveFocus();
    QKeyEvent discard(QEvent::KeyPress, Qt::Key_X, Qt::NoModifier, QStringLiteral("discard-this"));
    QCoreApplication::sendEvent(&window, &discard);
    QKeyEvent clear_line(QEvent::KeyPress, Qt::Key_U, Qt::ControlModifier, QStringLiteral("u"));
    QCoreApplication::sendEvent(&window, &clear_line);
    const QString command = QStringLiteral("printf '\\nLAPIS_INPUT_%s_OK\\n' %1; stty size")
                                .arg(QCoreApplication::applicationPid());
    for (const QChar character : command) {
        QKeyEvent event(QEvent::KeyPress, character.toUpper().unicode(), Qt::NoModifier,
                        QString(character));
        QCoreApplication::sendEvent(&window, &event);
    }
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier, QStringLiteral("\r"));
    QCoreApplication::sendEvent(&window, &enter);
}
class Capture final : public QObject {
  public:
    Capture(QQuickWindow& window, Workspace& workspace, UiPreview& preview, CaptureOptions options)
        : QObject(&window), window_(window), workspace_(workspace), preview_(preview),
          options_(std::move(options)) {
        clock_.start();
        frames_.reserve(512);
        connect(
            &window_, &QQuickWindow::frameSwapped, this, [this] { frame(); }, Qt::QueuedConnection);
        timeout_.setSingleShot(true);
        timeout_.setInterval(15000);
        connect(&timeout_, &QTimer::timeout, this, [] {
            qCritical("Window capture timed out waiting for a rendered frame or shell response");
            QCoreApplication::exit(1);
        });
        timeout_.start();
    }

  private:
    void frame() {
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
                send_smoke_input(window_);
                sent_ = true;
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
    new Capture(window, workspace, preview, std::move(options));
}
} // namespace lapis::desktop
