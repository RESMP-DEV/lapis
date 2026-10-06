#include "interaction_recorder.hpp"

#include "terminal_surface.hpp"
#include "workspace.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QGuiApplication>
#include <QInputMethodEvent>
#include <QJsonArray>
#include <QKeyEvent>
#include <QKeySequence>
#include <QMetaMethod>
#include <QMouseEvent>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRandomGenerator>
#include <QShortcutEvent>
#include <QWheelEvent>

#include <algorithm>
#include <unistd.h>
#include <utility>

namespace lapis::desktop {
namespace {
InteractionRecorder* g_active = nullptr;

// A focus change within this long of an input event is credited to it.
constexpr qint64 kCauseWindowUs = 1'000'000;
// History positions while scrubbing are recorded at most this often.
constexpr int kHistorySampleMs = 100;
// Pasted and copied text is kept up to this many characters.
constexpr qsizetype kTextLimit = 65'536;

QString elapsed_run_id() {
    return QString::number(QRandomGenerator::system()->generate64(), 16).rightJustified(16, u'0');
}

QJsonArray modifier_names(Qt::KeyboardModifiers modifiers) {
    QJsonArray names;
    // On macOS Qt reports Command as Control and Control as Meta unless
    // AA_MacDontSwapCtrlAndMeta is set, as lapis sets it; these are Qt's names.
    if (modifiers.testFlag(Qt::ShiftModifier))
        names.append(QStringLiteral("shift"));
    if (modifiers.testFlag(Qt::ControlModifier))
        names.append(QStringLiteral("control"));
    if (modifiers.testFlag(Qt::AltModifier))
        names.append(QStringLiteral("alt"));
    if (modifiers.testFlag(Qt::MetaModifier))
        names.append(QStringLiteral("meta"));
    if (modifiers.testFlag(Qt::KeypadModifier))
        names.append(QStringLiteral("keypad"));
    return names;
}

QString button_name(Qt::MouseButton button) {
    switch (button) {
    case Qt::LeftButton:
        return QStringLiteral("left");
    case Qt::RightButton:
        return QStringLiteral("right");
    case Qt::MiddleButton:
        return QStringLiteral("middle");
    case Qt::BackButton:
        return QStringLiteral("back");
    case Qt::ForwardButton:
        return QStringLiteral("forward");
    default:
        return QString::number(static_cast<int>(button));
    }
}

// The nearest object, this one or an item it sits in, that has a name.
QString named(const QObject* object) {
    for (const auto* item = qobject_cast<const QQuickItem*>(object); item != nullptr;
         item = item->parentItem())
        if (!item->objectName().isEmpty())
            return item->objectName();
    return object != nullptr ? object->objectName() : QString();
}

// Children from the top of the paint order down (z, then declaration).
QList<QQuickItem*> top_first(const QQuickItem& parent) {
    auto children = parent.childItems();
    std::reverse(children.begin(), children.end());
    std::stable_sort(
        children.begin(), children.end(),
        [](const QQuickItem* left, const QQuickItem* right) { return left->z() > right->z(); });
    return children;
}

// What a press at a scene point reaches, as Qt Quick picks it: the topmost
// visible, enabled item there that takes the mouse or hover; children may
// lie outside their parent unless it clips.
QQuickItem* pointer_target(QQuickItem* item, QPointF scene) {
    if (!item->isVisible() || !item->isEnabled())
        return nullptr;
    const auto local = item->mapFromScene(scene);
    if (item->clip() && !item->contains(local))
        return nullptr;
    for (auto* child : top_first(*item))
        if (auto* found = pointer_target(child, scene))
            return found;
    const bool takes = item->acceptedMouseButtons() != Qt::NoButton || item->acceptHoverEvents();
    return takes && item->contains(local) ? item : nullptr;
}

// The deepest visible item under a point, when nothing there takes input.
QQuickItem* item_at(QQuickItem* parent, QPointF point) {
    for (auto* child : top_first(*parent))
        if (child->isVisible() && child->contains(parent->mapToItem(child, point)))
            return item_at(child, parent->mapToItem(child, point));
    return parent;
}

// The text of the cursor's row, where a program asks for a password.
QString cursor_row(const session::TerminalSnapshot& snapshot) {
    if (!snapshot.cursor.in_viewport || snapshot.cursor.row >= snapshot.size.rows)
        return {};
    const std::size_t columns = snapshot.size.columns;
    QString text;
    for (std::size_t column = 0; column < columns; ++column) {
        const auto index = std::size_t{snapshot.cursor.row} * columns + column;
        if (index >= snapshot.cells.size())
            break;
        const auto value = snapshot.text(index);
        text += value.empty()
                    ? QStringLiteral(" ")
                    : QString::fromUcs4(value.data(), static_cast<qsizetype>(value.size()));
    }
    return text;
}

QString session_harness(const SessionPreview& item) {
    return item.harnessId().isEmpty() ? QStringLiteral("shell") : item.harnessId();
}

struct NamedArea {
    const char* prefix;
    const char* area;
};
constexpr NamedArea kSessionAreas[] = {
    {"agentTab_", "strip"},     {"cardPress_", "strip"}, {"previewTerminal_", "strip"},
    {"tileTerminal_", "stage"}, {"tilePress_", "stage"}, {"tileHeader_", "stage"},
    {"untile_", "stage"},       {"tile_", "stage"},      {"agentResult_", "search"},
};

// The part of the workspace a named item belongs to, and the agent its
// name carries, if any.
struct Area {
    QString area;
    QString session;
};
std::optional<Area> classify_area(const QString& name) {
    for (const auto& known : kSessionAreas)
        if (name.startsWith(QLatin1String(known.prefix)))
            return Area{QLatin1String(known.area),
                        name.mid(static_cast<qsizetype>(qstrlen(known.prefix)))};
    if (name.startsWith(QLatin1String("sideTerminal")))
        return Area{QStringLiteral("side-terminal"), {}};
    if (name == QLatin1String("liveTerminal") || name == QLatin1String("focusedPane"))
        return Area{QStringLiteral("stage"), {}};
    if (name.startsWith(QLatin1String("category")))
        return Area{QStringLiteral("categories"), {}};
    return std::nullopt;
}

// Credential-holding dialogs: nothing is recorded while one is open.
bool credential_dialog(const QString& name) {
    const auto lower = name.toLower();
    return name == QLatin1String("planSignInDialog") || name == QLatin1String("usageDialog") ||
           lower.contains(QLatin1String("signin")) || lower.contains(QLatin1String("account")) ||
           lower.contains(QLatin1String("credential")) || lower.contains(QLatin1String("password"));
}

QString capped(const QString& text, QJsonObject& record) {
    if (text.size() <= kTextLimit)
        return text;
    record.insert(QStringLiteral("truncated"), true);
    return text.left(kTextLimit);
}
} // namespace

InteractionRecorder::InteractionRecorder(Workspace& workspace, QString path, Hooks hooks)
    : workspace_(workspace), path_(std::move(path)), hooks_(std::move(hooks)),
      run_(elapsed_run_id()) {
    if (!hooks_.monotonicUs) {
        auto clock = std::make_shared<QElapsedTimer>();
        clock->start();
        hooks_.monotonicUs = [clock] { return clock->nsecsElapsed() / 1000; };
    }
    if (!hooks_.wallMs)
        hooks_.wallMs = [] { return QDateTime::currentMSecsSinceEpoch(); };
    if (!hooks_.secureInput)
        hooks_.secureInput = [] { return false; };
    pointer_timer_.setSingleShot(true);
    connect(&pointer_timer_, &QTimer::timeout, this, [this] { flushPointer(false); });
    wheel_timer_.setSingleShot(true);
    connect(&wheel_timer_, &QTimer::timeout, this, [this] {
        flushPointer(true);
        flushWheel();
    });
    history_timer_.setSingleShot(true);
    history_timer_.setInterval(kHistorySampleMs);
    connect(&history_timer_, &QTimer::timeout, this, [this] {
        if (!history_session_)
            return;
        note(QStringLiteral("history"),
             {{QStringLiteral("session"), history_session_->sessionId()},
              {QStringLiteral("active"), history_session_->historyActive()},
              {QStringLiteral("position"), history_session_->historyPosition()},
              {QStringLiteral("span"), history_session_->historySpan()},
              {QStringLiteral("via"), via()}});
    });
    followWorkspace();
    g_active = this;
}

InteractionRecorder::~InteractionRecorder() {
    if (g_active == this)
        g_active = nullptr;
    try {
        setSettings({}); // writes the stop mark and waits for the writer
    } catch (...) {
        // Shutdown must not throw; whatever was queued is already on its way.
        static_cast<void>(0);
    }
}

InteractionRecorder* InteractionRecorder::active() {
    return g_active != nullptr && g_active->recording() ? g_active : nullptr;
}

void InteractionRecorder::setSettings(const InteractionLogSettings& settings) {
    if (settings == settings_ && (writer_ != nullptr) == settings.enabled)
        return;
    if (writer_) {
        finishPending();
        flushPointer(true);
        flushWheel();
        writeNow({{QStringLiteral("kind"), QStringLiteral("stop")}});
        writer_.reset();
        if (QCoreApplication::instance() != nullptr)
            QCoreApplication::instance()->removeEventFilter(this);
    }
    settings_ = settings;
    sampler_.reset();
    if (!settings.enabled)
        return;
    writer_ = std::make_unique<InteractionWriter>(
        path_, InteractionWriter::Limits{.maxFileBytes = qint64{settings.maxFileMiB} * 1024 * 1024,
                                         .maxFiles = settings.maxFiles});
    sampler_.emplace(settings.pointerSampleMs);
    if (QCoreApplication::instance() != nullptr)
        QCoreApplication::instance()->installEventFilter(this);
    writeNow({{QStringLiteral("kind"), QStringLiteral("start")},
              {QStringLiteral("pid"), static_cast<qint64>(::getpid())},
              {QStringLiteral("pointerSampleMs"), settings.pointerSampleMs},
              {QStringLiteral("maxFileMiB"), settings.maxFileMiB},
              {QStringLiteral("maxFiles"), settings.maxFiles}});
}

void InteractionRecorder::drain() {
    finishPending();
    if (writer_)
        writer_->drain();
}

// Every record: format version, this launch, order, and both clocks.
void InteractionRecorder::writeNow(QJsonObject record, qint64 wall_ms) {
    if (!writer_)
        return;
    record.insert(QStringLiteral("v"), 1);
    record.insert(QStringLiteral("run"), run_);
    record.insert(QStringLiteral("seq"), static_cast<qint64>(++sequence_));
    if (!record.contains(QStringLiteral("mono_us")))
        record.insert(QStringLiteral("mono_us"), hooks_.monotonicUs());
    static_cast<void>(writer_->append(std::move(record), wall_ms < 0 ? hooks_.wallMs() : wall_ms));
}

// Other records go after any pointer sample or scroll they follow, and after
// a key press still waiting to learn what lapis did with it.
void InteractionRecorder::write(QJsonObject record) {
    if (!writer_ || suppressed())
        return;
    if (pending_ && !pending_->wheel) {
        record.insert(QStringLiteral("mono_us"), hooks_.monotonicUs());
        deferred_.push_back({std::move(record), hooks_.wallMs()});
        return;
    }
    if (pending_)
        finishPending();
    flushPointer(true);
    flushWheel();
    writeNow(std::move(record));
}

void InteractionRecorder::note(const QString& kind, QJsonObject fields) {
    fields.insert(QStringLiteral("kind"), kind);
    write(std::move(fields));
}

QString InteractionRecorder::via() const {
    const auto now = hooks_.monotonicUs();
    if (!explicit_via_.isEmpty() && now - explicit_via_us_ < kCauseWindowUs)
        return explicit_via_;
    if (!last_via_.isEmpty() && now - last_input_us_ < kCauseWindowUs)
        return last_via_;
    return QStringLiteral("program");
}

void InteractionRecorder::noteInput(const QString& via, const QString& detail) {
    last_via_ = via;
    last_detail_ = detail;
    last_input_us_ = hooks_.monotonicUs();
}

void InteractionRecorder::cause(const QString& via) {
    explicit_via_ = via;
    explicit_via_us_ = hooks_.monotonicUs();
}

const TerminalSurface* InteractionRecorder::focusedSurface() const {
    return qobject_cast<const TerminalSurface*>(QGuiApplication::focusObject());
}

// Why text from this terminal is withheld, or empty: macOS secure input is
// on, or the cursor's row asks for a secret.
QString InteractionRecorder::redaction(const TerminalSurface* surface) const {
    if (hooks_.secureInput())
        return QStringLiteral("secure-input");
    if (surface != nullptr && surface->document() != nullptr &&
        likely_secret_prompt(cursor_row(surface->document()->snapshot())))
        return QStringLiteral("secret-prompt");
    return {};
}

QJsonObject InteractionRecorder::focusContext() const {
    QJsonObject context;
    const auto* focus = QGuiApplication::focusObject();
    if (const auto* surface = qobject_cast<const TerminalSurface*>(focus);
        surface != nullptr && surface->document() != nullptr) {
        context.insert(QStringLiteral("surface"), surface->objectName());
        context.insert(QStringLiteral("session"), surface->document()->sessionId());
        context.insert(QStringLiteral("harness"), session_harness(*surface->document()));
    } else {
        context.insert(QStringLiteral("item"), named(focus));
    }
    if (const auto* agent = workspace_.focusedSession(); agent != nullptr) {
        context.insert(QStringLiteral("agent"), agent->sessionId());
        context.insert(QStringLiteral("agentHarness"), session_harness(*agent));
    }
    return context;
}

// What is under a point: the nearest named item, its named ancestors, and
// the part of the workspace with its agent where that is one.
QJsonObject InteractionRecorder::describeTarget(QQuickWindow* window, QPointF position) {
    QJsonObject target;
    if (window == nullptr || window->contentItem() == nullptr)
        return target;
    QJsonArray path;
    QString area;
    QString session;
    const QQuickItem* hit = pointer_target(window->contentItem(), position);
    if (hit == nullptr)
        hit = item_at(window->contentItem(), position);
    for (const QQuickItem* item = hit; item != nullptr; item = item->parentItem()) {
        if (const auto dialog = popup_items_.value(item); !dialog.isEmpty() && area.isEmpty()) {
            area = QStringLiteral("dialog");
            target.insert(QStringLiteral("dialog"), dialog);
        }
        if (const auto* surface = qobject_cast<const TerminalSurface*>(item);
            surface != nullptr && surface->document() != nullptr && session.isEmpty())
            session = surface->document()->sessionId();
        const auto& name = item->objectName();
        if (name.isEmpty())
            continue;
        if (path.size() < 4)
            path.append(name);
        if (const auto found = area.isEmpty() ? classify_area(name) : std::nullopt) {
            area = found->area;
            if (!found->session.isEmpty())
                session = found->session;
        }
    }
    if (!path.isEmpty())
        target.insert(QStringLiteral("name"), path.first());
    target.insert(QStringLiteral("path"), path);
    if (!area.isEmpty())
        target.insert(QStringLiteral("area"), area);
    if (!session.isEmpty())
        target.insert(QStringLiteral("session"), session);
    return target;
}

bool InteractionRecorder::eventFilter(QObject* watched, QEvent* event) {
    // Never consumes anything: every branch only reads the event.
    if (!writer_)
        return false;
    switch (event->type()) {
    case QEvent::ShortcutOverride:
    case QEvent::KeyPress:
    case QEvent::KeyRelease:
        keyEvent(watched, *static_cast<QKeyEvent*>(event),
                 event->type() == QEvent::ShortcutOverride);
        break;
    case QEvent::Shortcut:
        shortcutEvent(watched, *event);
        break;
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonRelease:
    case QEvent::MouseButtonDblClick:
        mouseEvent(watched, event);
        break;
    case QEvent::MouseMove:
        moveEvent(watched, event);
        break;
    case QEvent::Wheel:
        wheelEvent(watched, event);
        break;
    case QEvent::InputMethod:
        imeEvent(watched, event);
        break;
    case QEvent::WindowActivate:
    case QEvent::WindowDeactivate:
        windowEvent(watched, *event);
        break;
    default:
        break;
    }
    return false;
}

void InteractionRecorder::keyEvent(QObject* watched, const QKeyEvent& event,
                                   bool shortcut_override) {
    if (suppressed())
        return;
    const bool press = shortcut_override || event.type() == QEvent::KeyPress;
    if (press && pending_ && !pending_->wheel && pending_->key == event.key() &&
        pending_->timestamp == static_cast<qint64>(event.timestamp())) {
        // The same press reaching the window and then the focused item.
        if (!watched->isWindowType() && !pending_->record.contains(QStringLiteral("target")))
            pending_->record.insert(QStringLiteral("target"), named(watched));
        return;
    }
    if (!press && !watched->isWindowType())
        return; // a release is recorded once, at the window
    QJsonObject record{
        {QStringLiteral("kind"), QStringLiteral("key")},
        {QStringLiteral("type"), press ? QStringLiteral("press") : QStringLiteral("release")},
        {QStringLiteral("repeat"), event.isAutoRepeat()},
        {QStringLiteral("focus"), focusContext()}};
    const auto withheld = redaction(focusedSurface());
    const auto kind = key_class(event.key(), event.text());
    const bool secret_kind = kind == QLatin1String("character") || kind == QLatin1String("space") ||
                             kind == QLatin1String("other");
    if (!withheld.isEmpty() && secret_kind) {
        record.insert(QStringLiteral("redacted"), withheld);
        record.insert(QStringLiteral("class"), kind);
    } else {
        if (!withheld.isEmpty())
            record.insert(QStringLiteral("redacted"), withheld);
        record.insert(QStringLiteral("key"),
                      QKeySequence(event.key()).toString(QKeySequence::PortableText));
        record.insert(QStringLiteral("code"), event.key());
        record.insert(QStringLiteral("class"), kind);
        if (withheld.isEmpty() && !event.text().isEmpty())
            record.insert(QStringLiteral("text"), event.text());
        record.insert(QStringLiteral("mods"), modifier_names(event.modifiers()));
    }
    if (!watched->isWindowType())
        record.insert(QStringLiteral("target"), named(watched));
    if (!press) {
        finishPending(); // the press it ends goes first
        write(std::move(record));
        return;
    }
    finishPending();
    // A Shortcut event for this press, if any, comes next and names the action.
    noteInput(QStringLiteral("key"), record.value(QStringLiteral("key")).toString());
    record.insert(QStringLiteral("mono_us"), hooks_.monotonicUs());
    pending_ = Pending{std::move(record), event.key(), static_cast<qint64>(event.timestamp()),
                       false, hooks_.wallMs()};
    if (!finish_posted_) {
        finish_posted_ = true;
        QMetaObject::invokeMethod(this, &InteractionRecorder::finishPending, Qt::QueuedConnection);
    }
}

// A Shortcut in the window took the key: its object is named for the action.
void InteractionRecorder::shortcutEvent(QObject* watched, const QEvent& event) {
    if (suppressed())
        return;
    auto action = watched->objectName();
    if (action.endsWith(QLatin1String("Shortcut")))
        action.chop(8);
    const auto sequence =
        static_cast<const QShortcutEvent&>(event).key().toString(QKeySequence::PortableText);
    if (pending_ && !pending_->wheel) {
        pending_->record.insert(QStringLiteral("handled"), QStringLiteral("shortcut"));
        pending_->record.insert(QStringLiteral("action"), action);
        noteInput(QStringLiteral("shortcut"), action);
        return;
    }
    noteInput(QStringLiteral("shortcut"), action);
    note(QStringLiteral("shortcut"),
         {{QStringLiteral("action"), action}, {QStringLiteral("sequence"), sequence}});
}

void InteractionRecorder::flushWheel() {
    wheel_timer_.stop();
    if (!wheel_)
        return;
    QJsonObject record = std::move(*wheel_);
    wheel_.reset();
    writeNow(std::move(record));
}

void InteractionRecorder::finishPending() {
    finish_posted_ = false;
    explicit_via_.clear();
    if (!pending_)
        return;
    Pending finished = std::move(*pending_);
    pending_.reset();
    if (!finished.record.contains(QStringLiteral("handled")))
        finished.record.insert(QStringLiteral("handled"), QStringLiteral("delivered"));
    if (finished.wheel) {
        finishWheel(std::move(finished.record));
        return;
    }
    auto deferred = std::exchange(deferred_, {});
    if (suppressed())
        return;
    flushPointer(true);
    flushWheel();
    writeNow(std::move(finished.record), finished.wall_ms);
    for (auto& later : deferred)
        writeNow(std::move(later.record), later.wall_ms);
}

// A scroll gesture becomes one record per sample interval.
void InteractionRecorder::finishWheel(QJsonObject record) {
    const auto now = hooks_.monotonicUs();
    const int interval = settings_.pointerSampleMs > 0
                             ? settings_.pointerSampleMs
                             : InteractionLogSettings::kDefaultPointerSampleMs;
    const auto handled = record.value(QStringLiteral("handled"));
    if (wheel_ && wheel_->value(QStringLiteral("handled")) == handled &&
        now - wheel_started_us_ < qint64{interval} * 1000) {
        auto& open = *wheel_;
        for (const auto* field : {"dx", "dy", "pixelDx", "pixelDy", "amount"}) {
            const QString name = QLatin1String(field);
            open.insert(name, open.value(name).toInt() + record.value(name).toInt());
        }
        open.insert(QStringLiteral("folded"), open.value(QStringLiteral("folded")).toInt() + 1);
        return;
    }
    flushPointer(true);
    flushWheel();
    record.insert(QStringLiteral("folded"), 1);
    record.insert(QStringLiteral("mono_us"), now);
    wheel_ = std::move(record);
    wheel_started_us_ = now;
    wheel_timer_.start(interval);
}

void InteractionRecorder::mouseEvent(QObject* watched, QEvent* event) {
    auto* window = qobject_cast<QQuickWindow*>(watched);
    if (window == nullptr || suppressed())
        return;
    const auto& mouse = *static_cast<QMouseEvent*>(event);
    const auto type = event->type() == QEvent::MouseButtonPress ? QStringLiteral("press")
                      : event->type() == QEvent::MouseButtonRelease
                          ? QStringLiteral("release")
                          : QStringLiteral("double-click");
    auto target = describeTarget(window, mouse.position());
    if (event->type() == QEvent::MouseButtonPress)
        noteInput(QStringLiteral("click"), target.value(QStringLiteral("name")).toString());
    write({{QStringLiteral("kind"), QStringLiteral("mouse")},
           {QStringLiteral("type"), type},
           {QStringLiteral("button"), button_name(mouse.button())},
           {QStringLiteral("x"), mouse.position().x()},
           {QStringLiteral("y"), mouse.position().y()},
           {QStringLiteral("window"), window->objectName()},
           {QStringLiteral("mods"), modifier_names(mouse.modifiers())},
           {QStringLiteral("target"), target}});
}

void InteractionRecorder::moveEvent(QObject* watched, QEvent* event) {
    auto* window = qobject_cast<QQuickWindow*>(watched);
    if (window == nullptr || !sampler_ || suppressed())
        return;
    const auto& mouse = *static_cast<QMouseEvent*>(event);
    pointer_window_ = window;
    const auto now_ms = hooks_.monotonicUs() / 1000;
    if (const auto sample = sampler_->move(now_ms, mouse.position())) {
        flushWheel();
        writeNow({{QStringLiteral("kind"), QStringLiteral("pointer")},
                  {QStringLiteral("x"), sample->position.x()},
                  {QStringLiteral("y"), sample->position.y()},
                  {QStringLiteral("folded"), sample->folded},
                  {QStringLiteral("mono_us"), sample->at_ms * 1000},
                  {QStringLiteral("buttons"), static_cast<int>(mouse.buttons())},
                  {QStringLiteral("window"), window->objectName()},
                  {QStringLiteral("target"), describeTarget(window, sample->position)}});
        return;
    }
    schedulePointer();
}

void InteractionRecorder::schedulePointer() {
    if (!sampler_ || sampler_->due_ms() < 0 || pointer_timer_.isActive())
        return;
    const auto wait = sampler_->due_ms() - hooks_.monotonicUs() / 1000;
    pointer_timer_.start(static_cast<int>(std::clamp<qint64>(wait, 0, 10'000)));
}

void InteractionRecorder::flushPointer(bool force) {
    if (!sampler_ || suppressed())
        return;
    const auto sample = sampler_->flush(hooks_.monotonicUs() / 1000, force);
    if (!sample) {
        schedulePointer();
        return;
    }
    pointer_timer_.stop();
    writeNow(
        {{QStringLiteral("kind"), QStringLiteral("pointer")},
         {QStringLiteral("x"), sample->position.x()},
         {QStringLiteral("y"), sample->position.y()},
         {QStringLiteral("folded"), sample->folded},
         {QStringLiteral("mono_us"), sample->at_ms * 1000},
         {QStringLiteral("window"), pointer_window_ ? pointer_window_->objectName() : QString()},
         {QStringLiteral("target"), describeTarget(pointer_window_, sample->position)}});
}

void InteractionRecorder::wheelEvent(QObject* watched, QEvent* event) {
    auto* window = qobject_cast<QQuickWindow*>(watched);
    if (window == nullptr || suppressed())
        return;
    const auto& wheel = *static_cast<QWheelEvent*>(event);
    finishPending();
    QJsonObject record{{QStringLiteral("kind"), QStringLiteral("wheel")},
                       {QStringLiteral("dx"), wheel.angleDelta().x()},
                       {QStringLiteral("dy"), wheel.angleDelta().y()},
                       {QStringLiteral("pixelDx"), wheel.pixelDelta().x()},
                       {QStringLiteral("pixelDy"), wheel.pixelDelta().y()},
                       {QStringLiteral("x"), wheel.position().x()},
                       {QStringLiteral("y"), wheel.position().y()},
                       {QStringLiteral("inverted"), wheel.inverted()},
                       {QStringLiteral("window"), window->objectName()}};
    // The target is looked up once per gesture, when its record starts.
    if (!wheel_)
        record.insert(QStringLiteral("target"), describeTarget(window, wheel.position()));
    pending_ = Pending{std::move(record), 0, static_cast<qint64>(wheel.timestamp()), true,
                       hooks_.wallMs()};
    noteInput(QStringLiteral("wheel"), {});
    if (!finish_posted_) {
        finish_posted_ = true;
        QMetaObject::invokeMethod(this, &InteractionRecorder::finishPending, Qt::QueuedConnection);
    }
}

void InteractionRecorder::imeEvent(QObject* watched, QEvent* event) {
    if (watched->isWindowType() || suppressed())
        return;
    const auto& ime = *static_cast<QInputMethodEvent*>(event);
    const bool ended = composing_ && ime.preeditString().isEmpty();
    composing_ = !ime.preeditString().isEmpty();
    if (ime.preeditString().isEmpty() && ime.commitString().isEmpty() && !ended)
        return;
    QJsonObject record{
        {QStringLiteral("kind"), QStringLiteral("ime")},
        {QStringLiteral("target"), named(watched)},
        {QStringLiteral("focus"), focusContext()},
        {QStringLiteral("preeditLength"), static_cast<qint64>(ime.preeditString().size())},
        {QStringLiteral("commitLength"), static_cast<qint64>(ime.commitString().size())}};
    if (const auto withheld = redaction(qobject_cast<const TerminalSurface*>(watched));
        !withheld.isEmpty()) {
        record.insert(QStringLiteral("redacted"), withheld);
    } else {
        record.insert(QStringLiteral("preedit"), ime.preeditString());
        record.insert(QStringLiteral("commit"), ime.commitString());
    }
    write(std::move(record));
}

void InteractionRecorder::windowEvent(QObject* watched, const QEvent& event) {
    if (!watched->isWindowType())
        return;
    note(QStringLiteral("window"), {{QStringLiteral("event"), event.type() == QEvent::WindowActivate
                                                                  ? QStringLiteral("activate")
                                                                  : QStringLiteral("deactivate")},
                                    {QStringLiteral("window"), watched->objectName()}});
}

void InteractionRecorder::keyOutcome(const QString& handled, const QJsonObject& detail) {
    if (!pending_ || pending_->wheel)
        return;
    if (!handled.isEmpty())
        pending_->record.insert(QStringLiteral("handled"), handled);
    for (auto field = detail.begin(); field != detail.end(); ++field)
        pending_->record.insert(field.key(), field.value());
}

void InteractionRecorder::wheelOutcome(const QString& handled, int amount) {
    if (!pending_ || !pending_->wheel)
        return;
    pending_->record.insert(QStringLiteral("handled"), handled);
    pending_->record.insert(QStringLiteral("amount"), amount);
}

void InteractionRecorder::paste(const TerminalSurface* surface, const QString& text, bool accepted,
                                const char* source) {
    QJsonObject record{{QStringLiteral("kind"), QStringLiteral("paste")},
                       {QStringLiteral("source"), QLatin1String(source)},
                       {QStringLiteral("length"), static_cast<qint64>(text.size())},
                       {QStringLiteral("accepted"), accepted}};
    if (surface != nullptr && surface->document() != nullptr) {
        record.insert(QStringLiteral("session"), surface->document()->sessionId());
        record.insert(QStringLiteral("harness"), session_harness(*surface->document()));
        record.insert(QStringLiteral("surface"), surface->objectName());
    }
    if (const auto withheld = redaction(surface); !withheld.isEmpty())
        record.insert(QStringLiteral("redacted"), withheld);
    else
        record.insert(QStringLiteral("text"), capped(text, record));
    write(std::move(record));
}

void InteractionRecorder::copy(const TerminalSurface* surface, const QString& text,
                               const char* how) {
    QJsonObject record{{QStringLiteral("kind"), QStringLiteral("copy")},
                       {QStringLiteral("how"), QLatin1String(how)},
                       {QStringLiteral("length"), static_cast<qint64>(text.size())}};
    if (surface != nullptr && surface->document() != nullptr)
        record.insert(QStringLiteral("session"), surface->document()->sessionId());
    record.insert(QStringLiteral("text"), capped(text, record));
    write(std::move(record));
}

void InteractionRecorder::followWorkspace() {
    focused_id_ =
        workspace_.focusedSession() ? workspace_.focusedSession()->sessionId() : QString();
    category_id_ = workspace_.activeCategoryId();
    followHistory();
    connect(&workspace_, &Workspace::focusChanged, this, &InteractionRecorder::agentFocused);
    connect(&workspace_, &Workspace::categoryChanged, this, &InteractionRecorder::categorySelected);
    connect(&workspace_, &Workspace::tilesChanged, this, &InteractionRecorder::tilesChanged);
}

void InteractionRecorder::agentFocused() {
    const auto* item = workspace_.focusedSession();
    const auto id = item ? item->sessionId() : QString();
    if (id == focused_id_)
        return;
    focused_id_ = id;
    followHistory();
    if (!recording())
        return;
    const auto how = via();
    QJsonObject record{{QStringLiteral("session"), id},
                       {QStringLiteral("via"), how},
                       {QStringLiteral("category"), workspace_.activeCategoryId()}};
    if (item != nullptr) {
        record.insert(QStringLiteral("harness"), session_harness(*item));
        record.insert(QStringLiteral("title"), item->title());
    }
    // The shortcut, key or clicked item behind it.
    if (how == last_via_ && !last_detail_.isEmpty())
        record.insert(QStringLiteral("detail"), last_detail_);
    note(QStringLiteral("agent-focus"), record);
}

void InteractionRecorder::categorySelected() {
    if (workspace_.activeCategoryId() == category_id_)
        return;
    category_id_ = workspace_.activeCategoryId();
    if (!recording())
        return;
    QString name;
    for (const auto& value : workspace_.categories())
        if (const auto map = value.toMap();
            map.value(QStringLiteral("id")).toString() == category_id_)
            name = map.value(QStringLiteral("name")).toString();
    note(QStringLiteral("category"), {{QStringLiteral("category"), category_id_},
                                      {QStringLiteral("name"), name},
                                      {QStringLiteral("via"), via()}});
}

void InteractionRecorder::tilesChanged() {
    if (!recording())
        return;
    QStringList tiles;
    for (const auto& value : workspace_.stageTiles())
        tiles.append(value.toMap().value(QStringLiteral("sessionId")).toString());
    if (tiles == tiles_)
        return;
    tiles_ = tiles;
    note(QStringLiteral("tiles"), {{QStringLiteral("tiles"), QJsonArray::fromStringList(tiles)},
                                   {QStringLiteral("category"), workspace_.activeCategoryId()},
                                   {QStringLiteral("via"), via()}});
}

// The focused agent's history position: scrubbing, paging and returning live.
void InteractionRecorder::followHistory() {
    disconnect(history_connection_);
    history_session_ = workspace_.focusedSession();
    if (history_session_)
        history_connection_ =
            connect(history_session_.data(), &SessionPreview::historyChanged, this, [this] {
                if (recording() && !history_timer_.isActive())
                    history_timer_.start();
            });
}

void InteractionRecorder::watchWindow(QQuickWindow* window) {
    if (window == nullptr)
        return;
    refreshPopups(window);
}

void InteractionRecorder::refreshPopups(QQuickWindow* window) {
    const int slot = metaObject()->indexOfSlot("popupVisibilityChanged()");
    for (auto* object : window->findChildren<QObject*>()) {
        if (!object->inherits("QQuickPopup") || object->objectName().isEmpty())
            continue;
        if (std::any_of(popups_.cbegin(), popups_.cend(),
                        [object](const QPointer<QObject>& known) { return known == object; }))
            continue;
        popups_.append(object);
        const int changed = object->metaObject()->indexOfSignal("visibleChanged()");
        if (changed >= 0)
            connect(object, object->metaObject()->method(changed), this,
                    metaObject()->method(slot));
        if (const auto* content = object->property("contentItem").value<QQuickItem*>();
            content != nullptr && content->parentItem() != nullptr)
            popup_items_.insert(content->parentItem(), object->objectName());
        if (object->objectName() == QLatin1String("commandsDialog"))
            connect(object, SIGNAL(requested(QString)), this, SLOT(commandRequested(QString)));
    }
}

void InteractionRecorder::popupVisibilityChanged() {
    auto* popup = sender();
    if (popup == nullptr)
        return;
    const auto name = popup->objectName();
    const bool open = popup->property("visible").toBool();
    if (const auto* content = popup->property("contentItem").value<QQuickItem*>();
        content != nullptr && content->parentItem() != nullptr)
        popup_items_.insert(content->parentItem(), name);
    if (credential_dialog(name)) {
        if (open && !credential_dialogs_.contains(name)) {
            finishPending();
            write({{QStringLiteral("kind"), QStringLiteral("dialog")},
                   {QStringLiteral("name"), name},
                   {QStringLiteral("open"), true}});
            if (sampler_)
                sampler_->reset();
            credential_dialogs_.append(name);
            suppressed_since_us_ = hooks_.monotonicUs();
        } else if (!open && credential_dialogs_.removeAll(name) > 0 && !suppressed()) {
            // The gap is noted once the dialog is gone; nothing from inside it.
            note(QStringLiteral("dialog"),
                 {{QStringLiteral("name"), name},
                  {QStringLiteral("open"), false},
                  {QStringLiteral("unrecordedMs"),
                   (hooks_.monotonicUs() - suppressed_since_us_) / 1000}});
        }
        return;
    }
    note(QStringLiteral("dialog"), {{QStringLiteral("name"), name},
                                    {QStringLiteral("open"), open},
                                    {QStringLiteral("via"), via()}});
}

void InteractionRecorder::commandRequested(const QString& id) {
    note(QStringLiteral("command"),
         {{QStringLiteral("id"), id}, {QStringLiteral("via"), QStringLiteral("palette")}});
}

namespace interaction {
void key_outcome(const QString& handled, const QJsonObject& detail) {
    if (auto* recorder = InteractionRecorder::active())
        recorder->keyOutcome(handled, detail);
}
void paste(const TerminalSurface* surface, const QString& text, bool accepted, const char* source) {
    if (auto* recorder = InteractionRecorder::active())
        recorder->paste(surface, text, accepted, source);
}
void wheel_outcome(const QString& handled, int amount) {
    if (auto* recorder = InteractionRecorder::active())
        recorder->wheelOutcome(handled, amount);
}
void copy(const TerminalSurface* surface, const QString& text, const char* how) {
    if (auto* recorder = InteractionRecorder::active())
        recorder->copy(surface, text, how);
}
void cause(const QString& via) {
    if (auto* recorder = InteractionRecorder::active())
        recorder->cause(via);
}
} // namespace interaction

} // namespace lapis::desktop
