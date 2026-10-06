#ifndef LAPIS_DESKTOP_INTERACTION_RECORDER_HPP
#define LAPIS_DESKTOP_INTERACTION_RECORDER_HPP

#include "interaction_log.hpp"

#include <QElapsedTimer>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

class QEvent;
class QKeyEvent;
class QQuickItem;
class QQuickWindow;

namespace lapis::desktop {
class SessionPreview;
class TerminalSurface;
class Workspace;

// Records what the person does in lapis's windows (see interaction_log.hpp):
// one application event filter sees every key, IME, mouse and wheel event,
// and the places where lapis decides what a key did report that through the
// hooks in `interaction`. Only the real workspace makes one; previews and
// test fixtures have none, and every hook is then a no-op.
//
// GUI thread only. The filter never consumes an event and builds each
// record in a few microseconds; serializing and writing happen on the
// writer's thread.
class InteractionRecorder : public QObject {
    Q_OBJECT
  public:
    struct Hooks {
        // Whether macOS secure event input is on (a password field anywhere).
        std::function<bool()> secureInput;
        // Monotonic microseconds and wall milliseconds; tests replace them.
        std::function<qint64()> monotonicUs;
        std::function<qint64()> wallMs;
    };
    InteractionRecorder(Workspace& workspace, QString path, Hooks hooks = {});
    ~InteractionRecorder() override;
    InteractionRecorder(const InteractionRecorder&) = delete;
    InteractionRecorder& operator=(const InteractionRecorder&) = delete;
    InteractionRecorder(InteractionRecorder&&) = delete;
    InteractionRecorder& operator=(InteractionRecorder&&) = delete;

    // The recorder the hooks report to, or null.
    [[nodiscard]] static InteractionRecorder* active();
    // Starts or stops recording, reopening the writer when its limits change.
    void setSettings(const InteractionLogSettings& settings);
    [[nodiscard]] bool recording() const { return writer_ != nullptr; }
    // Follows the window's dialogs and menus (open and close, the command
    // run from the palette) and holds recording while a credential dialog
    // is open.
    void watchWindow(QQuickWindow* window);
    // Everything queued so far is on disk.
    void drain();

    // Hooks; see `interaction` below.
    void keyOutcome(const QString& handled, const QJsonObject& detail);
    void paste(const TerminalSurface* surface, const QString& text, bool accepted,
               const char* source);
    void wheelOutcome(const QString& handled, int amount);
    void copy(const TerminalSurface* surface, const QString& text, const char* how);
    void cause(const QString& via);
    void note(const QString& kind, QJsonObject fields);

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

  private slots:
    void popupVisibilityChanged();
    void commandRequested(const QString& id);

  private:
    struct Pending {
        QJsonObject record;
        int key{};
        qint64 timestamp{};
        bool wheel{};
        qint64 wall_ms{};
    };
    struct Deferred {
        QJsonObject record;
        qint64 wall_ms{};
    };
    void write(QJsonObject record);
    void writeNow(QJsonObject record, qint64 wall_ms = -1);
    void finishPending();
    void finishWheel(QJsonObject record);
    void flushWheel();
    void flushPointer(bool force);
    void schedulePointer();
    void keyEvent(QObject* watched, const QKeyEvent& event, bool shortcut_override);
    void shortcutEvent(QObject* watched, const QEvent& event);
    void mouseEvent(QObject* watched, QEvent* event);
    void wheelEvent(QObject* watched, QEvent* event);
    void moveEvent(QObject* watched, QEvent* event);
    void imeEvent(QObject* watched, QEvent* event);
    void windowEvent(QObject* watched, const QEvent& event);
    [[nodiscard]] QString redaction(const TerminalSurface* surface) const;
    [[nodiscard]] const TerminalSurface* focusedSurface() const;
    [[nodiscard]] QJsonObject focusContext() const;
    [[nodiscard]] QJsonObject describeTarget(QQuickWindow* window, QPointF position);
    void refreshPopups(QQuickWindow* window);
    [[nodiscard]] bool suppressed() const { return !credential_dialogs_.isEmpty(); }
    [[nodiscard]] QString via() const;
    void noteInput(const QString& via, const QString& detail);
    void followWorkspace();
    void agentFocused();
    void categorySelected();
    void tilesChanged();
    void followHistory();

    Workspace& workspace_;
    QString path_;
    Hooks hooks_;
    InteractionLogSettings settings_;
    std::unique_ptr<InteractionWriter> writer_;
    QString run_;
    quint64 sequence_{};
    std::optional<Pending> pending_;
    // Records that came while a key press waited for its outcome; they
    // follow it, keeping the moment they happened.
    std::vector<Deferred> deferred_;
    bool finish_posted_{};
    std::optional<PointerSampler> sampler_;
    QPointer<QQuickWindow> pointer_window_;
    QTimer pointer_timer_;
    // A wheel record collecting a scroll gesture until the sample interval.
    std::optional<QJsonObject> wheel_;
    qint64 wheel_started_us_{};
    QTimer wheel_timer_;
    QList<QPointer<QObject>> popups_;
    QHash<const QQuickItem*, QString> popup_items_;
    bool composing_{};
    QList<QString> credential_dialogs_;
    qint64 suppressed_since_us_{};
    // The latest input and an explicit cause, to say how a focus change came.
    QString last_via_;
    QString last_detail_;
    qint64 last_input_us_{};
    QString explicit_via_;
    qint64 explicit_via_us_{};
    QPointer<SessionPreview> history_session_;
    QMetaObject::Connection history_connection_;
    QTimer history_timer_;
    QString focused_id_;
    QString category_id_;
    QStringList tiles_;
};

// What lapis did with input, reported where it decides. Each is a no-op
// unless the real workspace is recording.
namespace interaction {
// The key being handled went to the agent ("agent"), or lapis took it:
// "copy", "paste", "suggestion-fill", "suggestion-send", "tab-away",
// "held-for-request", "history-live", "ime", "ignored".
void key_outcome(const QString& handled, const QJsonObject& detail = {});
// A paste into a terminal: Command-V, a dropped file, a Tab-filled guess.
void paste(const TerminalSurface* surface, const QString& text, bool accepted, const char* source);
// The wheel scrolled lapis's kept history (`rows`) or went to the program
// (`steps`).
void wheel_outcome(const QString& handled, int amount);
// Text copied from a terminal selection ("key", "select", "double-click").
void copy(const TerminalSurface* surface, const QString& text, const char* how);
// The next focus change came from this ("tab-away", "notification",
// "attention-key", "terminal-key").
void cause(const QString& via);
} // namespace interaction

} // namespace lapis::desktop
#endif // LAPIS_DESKTOP_INTERACTION_RECORDER_HPP
