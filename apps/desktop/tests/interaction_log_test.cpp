#include "interaction_log.hpp"
#include "interaction_recorder.hpp"
#include "keymap.hpp"
#include "platform_desktop.hpp"
#include "terminal_surface.hpp"
#include "ui_preview.hpp"
#include "workspace.hpp"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QInputMethodEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QTemporaryDir>
#include <QThread>
#include <QWheelEvent>

#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace {
using lapis::desktop::InteractionLogSettings;
using lapis::desktop::InteractionRecorder;
using lapis::desktop::InteractionWriter;
using lapis::desktop::PointerSampler;

void require(bool condition, const std::string& message) {
    if (!condition)
        throw std::runtime_error(message);
}

std::vector<QJsonObject> read_lines(const QString& path) {
    QFile file(path);
    std::vector<QJsonObject> lines;
    if (!file.open(QIODevice::ReadOnly))
        return lines;
    for (const auto& line : file.readAll().split('\n'))
        if (!line.isEmpty())
            lines.push_back(QJsonDocument::fromJson(line).object());
    return lines;
}

std::vector<QJsonObject> of_kind(const std::vector<QJsonObject>& lines, const QString& kind) {
    std::vector<QJsonObject> found;
    for (const auto& line : lines)
        if (line.value(QStringLiteral("kind")).toString() == kind)
            found.push_back(line);
    return found;
}

mode_t mode_of(const QString& path) {
    struct stat info{};
    require(::stat(QFile::encodeName(path).constData(), &info) == 0, "stat failed");
    return info.st_mode & static_cast<mode_t>(S_IRWXU | S_IRWXG | S_IRWXO);
}

void settings_parse_with_defaults_and_limits() {
    const auto defaults = lapis::desktop::parse_interaction_log(QJsonValue());
    require(!defaults.enabled && defaults.pointerSampleMs == 50 && defaults.maxFileMiB == 64 &&
                defaults.maxFiles == 8,
            "the log is off by default with its documented limits");
    const auto set = lapis::desktop::parse_interaction_log(
        QJsonDocument::fromJson(
            R"({"enabled": true, "pointerSampleMs": 20, "maxFileMiB": 2, "maxFiles": 3})")
            .object());
    require(set.enabled && set.pointerSampleMs == 20 && set.maxFileMiB == 2 && set.maxFiles == 3,
            "each setting is read");
    const auto clamped = lapis::desktop::parse_interaction_log(
        QJsonDocument::fromJson(
            R"({"enabled": "yes", "pointerSampleMs": -5, "maxFileMiB": 99999, "maxFiles": 0})")
            .object());
    require(!clamped.enabled && clamped.pointerSampleMs == 0 && clamped.maxFileMiB == 1024 &&
                clamped.maxFiles == 1,
            "a non-boolean switch stays off and numbers are clamped to their limits");
}

void secret_prompts_are_recognized() {
    for (const auto* row :
         {"Password:", "[sudo] password for someone: ",
          "Enter passphrase for key '/x/id_ed25519': ", "Enter PIN: ", "OTP code",
          "Verification code: ", "Paste your API key:", "Token: ", "PASSCODE", "one-time code"})
        require(lapis::desktop::likely_secret_prompt(QString::fromUtf8(row)),
                std::string("a secret prompt: ") + row);
    for (const auto* row : {"> fix the spinning loader", "pinned tabs", "keyboard layout",
                            "monkey: ", "  Working (12s)", ""})
        require(!lapis::desktop::likely_secret_prompt(QString::fromUtf8(row)),
                std::string("not a secret prompt: ") + row);
}

void pointer_moves_are_sampled_and_coalesced() {
    PointerSampler sampler(50);
    require(sampler.move(0, {1, 1}).has_value(), "the first move is recorded at once");
    require(!sampler.move(10, {2, 2}) && !sampler.move(20, {3, 3}) && !sampler.move(30, {4, 4}),
            "moves within the interval wait");
    require(sampler.due_ms() == 50, "the waiting sample is due one interval after the last");
    require(!sampler.flush(40, false), "not before it is due");
    const auto trailing = sampler.flush(50, false);
    require(trailing && trailing->position == QPointF(4, 4) && trailing->folded == 3 &&
                trailing->at_ms == 30,
            "the trailing sample carries the latest position and how many moves it stands for");
    require(sampler.due_ms() < 0, "nothing waits after a flush");
    require(!sampler.move(60, {5, 5}), "a move right after a sample waits");
    const auto late = sampler.move(120, {6, 6});
    require(late && late->folded == 2 && late->position == QPointF(6, 6),
            "a move after the interval records the folded sample at once");
    PointerSampler off(0);
    require(!off.move(0, {1, 1}) && !off.move(1000, {2, 2}), "0 records no movement");
}

void writer_formats_rotates_and_stays_private() {
    QTemporaryDir directory;
    const auto folder = directory.filePath(QStringLiteral("runtime"));
    const auto path = folder + QStringLiteral("/interaction.jsonl");
    {
        InteractionWriter writer(path, {.maxFileBytes = 1024, .maxFiles = 3});
        for (int index = 0; index < 60; ++index)
            require(writer.append({{QStringLiteral("kind"), QStringLiteral("test")},
                                   {QStringLiteral("seq"), index},
                                   {QStringLiteral("pad"), QString(40, u'x')}},
                                  1'700'000'000'000 + index),
                    "the record is queued");
        writer.drain();
        require(writer.dropped() == 0 && writer.failures() == 0, "nothing was lost");
    }
    require(mode_of(folder) == 0700, "the folder is owner-only");
    require(mode_of(path) == 0600, "the log is owner-only");
    require(QFileInfo::exists(path + ".1") && QFileInfo::exists(path + ".2") &&
                !QFileInfo::exists(path + ".3"),
            "rotation keeps the configured number of files");
    for (const auto& name : {path, path + ".1", path + ".2"}) {
        require(QFileInfo(name).size() <= 1024, "no file passes its cap");
        require(mode_of(name) == 0600, "rotated files stay owner-only");
    }
    const auto current = read_lines(path);
    const auto previous = read_lines(path + ".1");
    require(!current.empty() && !previous.empty(), "both files hold records");
    require(current.back().value(QStringLiteral("seq")).toInt() == 59,
            "the newest record is in the current file");
    require(previous.back().value(QStringLiteral("seq")).toInt() + 1 ==
                current.front().value(QStringLiteral("seq")).toInt(),
            "the current file continues where its predecessor ended");
    require(current.back().value(QStringLiteral("wall")).toString().endsWith(u'Z'),
            "wall time is ISO 8601 UTC");
}

void writer_refuses_a_shared_folder() {
    QTemporaryDir directory;
    const auto folder = directory.filePath(QStringLiteral("runtime"));
    require(QDir().mkpath(folder), "fixture folder");
    require(::chmod(QFile::encodeName(folder).constData(), 0755) == 0, "fixture permissions");
    const auto path = folder + QStringLiteral("/interaction.jsonl");
    InteractionWriter writer(path, {.maxFileBytes = 1024, .maxFiles = 2});
    require(writer.append({{QStringLiteral("kind"), QStringLiteral("test")}}, 0), "queued");
    writer.drain();
    require(!QFileInfo::exists(path) && writer.failures() == 1,
            "a folder others can read is refused, not changed");
    require(mode_of(folder) == 0755, "the folder is left as it was");
}

struct Fixture {
    QTemporaryDir directory;
    QString path = directory.filePath(QStringLiteral("runtime/interaction.jsonl"));
    lapis::desktop::Workspace workspace{lapis::desktop::WorkspaceMode::preview};
    qint64 now_us = 1'000'000;
    bool secure = false;
    std::unique_ptr<InteractionRecorder> recorder = std::make_unique<InteractionRecorder>(
        workspace, path,
        InteractionRecorder::Hooks{.secureInput = [this] { return secure; },
                                   .monotonicUs = [this] { return now_us; },
                                   .wallMs = [] { return qint64{1'700'000'000'000}; }});
    QQuickWindow window;
    ulong stamp = 1;

    void key(QEvent::Type type, int code, const QString& text,
             Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        QKeyEvent event(type, code, modifiers, text);
        event.setTimestamp(stamp++);
        QCoreApplication::sendEvent(&window, &event);
    }
    void move(QPointF at) {
        QMouseEvent event(QEvent::MouseMove, at, at, Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(&window, &event);
    }
    std::vector<QJsonObject> lines() {
        recorder->drain();
        return read_lines(path);
    }
};

void disabled_writes_nothing() {
    Fixture fixture;
    fixture.key(QEvent::KeyPress, Qt::Key_A, QStringLiteral("a"));
    fixture.move({10, 10});
    fixture.recorder->drain();
    require(!fixture.recorder->recording() && !QFileInfo::exists(fixture.path) &&
                InteractionRecorder::active() == nullptr,
            "with the setting off nothing is recorded and no file appears");
    fixture.recorder->setSettings({.enabled = true});
    fixture.recorder->setSettings({});
    const auto lines = fixture.lines();
    require(lines.size() == 2 && lines.front().value("kind") == "start" &&
                lines.back().value("kind") == "stop",
            "turning it on and off writes only the start and stop marks");
}

void records_keys_ime_and_paste() {
    Fixture fixture;
    fixture.recorder->setSettings({.enabled = true});
    fixture.key(QEvent::KeyPress, Qt::Key_A, QStringLiteral("a"));
    fixture.key(QEvent::KeyRelease, Qt::Key_A, QStringLiteral("a"));
    fixture.key(QEvent::KeyPress, Qt::Key_K, QStringLiteral("k"), Qt::MetaModifier);
    lapis::desktop::interaction::paste(nullptr, QStringLiteral("hello"), true, "paste");
    QInputMethodEvent commit;
    commit.setCommitString(QStringLiteral("dictated words"));
    QObject item;
    QCoreApplication::sendEvent(&item, &commit);
    const auto lines = fixture.lines();
    const auto keys = of_kind(lines, QStringLiteral("key"));
    require(keys.size() == 3, "two presses and a release");
    const auto& press = keys[0];
    require(press.value("type") == "press" && press.value("key") == "A" &&
                press.value("text") == "a" && press.value("class") == "character" &&
                press.value("handled") == "delivered" && press.value("v").toInt() == 1 &&
                press.value("run").toString().size() == 16 && press.contains("mono_us") &&
                press.contains("wall") && press.contains("focus"),
            "a key press with its text, class, outcome and both clocks");
    require(keys[1].value("type") == "release", "the release is its own record");
    require(keys[2].value("mods").toArray().contains(QStringLiteral("meta")),
            "modifiers are named");
    std::int64_t previous = 0;
    for (const auto& line : lines) {
        require(line.value("seq").toInteger() == previous + 1, "sequence numbers are gapless");
        previous = line.value("seq").toInteger();
    }
    const auto pastes = of_kind(lines, QStringLiteral("paste"));
    require(pastes.size() == 1 && pastes[0].value("text") == "hello" &&
                pastes[0].value("length").toInt() == 5,
            "a paste keeps its length and text");
    const auto ime = of_kind(lines, QStringLiteral("ime"));
    require(ime.size() == 1 && ime[0].value("commit") == "dictated words",
            "an IME or dictation commit is recorded");
}

void secret_input_is_withheld() {
    Fixture fixture;
    fixture.recorder->setSettings({.enabled = true});
    fixture.secure = true;
    fixture.key(QEvent::KeyPress, Qt::Key_S, QStringLiteral("s"));
    fixture.key(QEvent::KeyPress, Qt::Key_Return, QStringLiteral("\r"));
    lapis::desktop::interaction::paste(nullptr, QStringLiteral("hunter2"), true, "paste");
    QInputMethodEvent commit;
    commit.setCommitString(QStringLiteral("secret"));
    QObject item;
    QCoreApplication::sendEvent(&item, &commit);
    const auto lines = fixture.lines();
    const QByteArray raw = [&] {
        QFile file(fixture.path);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    }();
    require(!raw.contains("hunter2") && !raw.contains("\"s\"") && !raw.contains("secret\""),
            "no secret text reaches the file");
    const auto keys = of_kind(lines, QStringLiteral("key"));
    require(keys.size() == 2 && keys[0].value("redacted") == "secure-input" &&
                keys[0].value("class") == "character" && !keys[0].contains("key") &&
                !keys[0].contains("text"),
            "a character typed under secure input keeps only its class");
    require(keys[1].value("key") == "Return" && !keys[1].contains("text"),
            "Return is still named, without text");
    const auto pastes = of_kind(lines, QStringLiteral("paste"));
    require(pastes.size() == 1 && pastes[0].value("length").toInt() == 7 &&
                !pastes[0].contains("text"),
            "a withheld paste keeps its length");
}

void pointer_and_wheel_are_coalesced() {
    Fixture fixture;
    fixture.recorder->setSettings({.enabled = true, .pointerSampleMs = 50});
    for (int step = 0; step < 10; ++step) {
        fixture.move({10.0 + step, 20});
        fixture.now_us += 10'000; // 10 ms apart
    }
    QCoreApplication::processEvents();
    for (int step = 0; step < 5; ++step) {
        QWheelEvent wheel({5, 5}, {5, 5}, {0, 3}, {0, 120}, Qt::NoButton, Qt::NoModifier,
                          Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(&fixture.window, &wheel);
        QCoreApplication::processEvents();
        fixture.now_us += 5'000;
    }
    fixture.key(QEvent::KeyPress, Qt::Key_B, QStringLiteral("b"));
    const auto lines = fixture.lines();
    const auto pointer = of_kind(lines, QStringLiteral("pointer"));
    int folded = 0;
    for (const auto& line : pointer)
        folded += line.value("folded").toInt();
    require(pointer.size() >= 2 && pointer.size() <= 3 && folded == 10,
            "ten moves over 100 ms become two or three records that account for all of them");
    require(pointer.back().value("x").toDouble() == 19, "the last position is kept");
    const auto wheel = of_kind(lines, QStringLiteral("wheel"));
    require(wheel.size() == 1 && wheel[0].value("folded").toInt() == 5 &&
                wheel[0].value("dy").toInt() == 600 && wheel[0].value("pixelDy").toInt() == 15,
            "one scroll gesture is one record with summed deltas");
    const auto keys = of_kind(lines, QStringLiteral("key"));
    require(!keys.empty() &&
                keys.back().value("seq").toInteger() > wheel.back().value("seq").toInteger(),
            "records stay in the order things happened");
}

void credential_dialogs_pause_recording() {
    Fixture fixture;
    fixture.recorder->setSettings({.enabled = true});
    QQmlEngine engine;
    QQmlComponent component(&engine);
    component.setData(R"(import QtQuick
import QtQuick.Controls
Window {
    width: 400; height: 300
    Dialog { objectName: "planSignInDialog"; width: 100; height: 100 }
    Dialog { objectName: "commandsDialog"; width: 100; height: 100
             signal requested(string commandId) }
})",
                      QUrl());
    std::unique_ptr<QObject> root(component.create());
    require(root != nullptr, component.errorString().toStdString());
    auto* window = qobject_cast<QQuickWindow*>(root.get());
    fixture.recorder->watchWindow(window);
    auto* sign_in = root->findChild<QObject*>(QStringLiteral("planSignInDialog"));
    auto* commands = root->findChild<QObject*>(QStringLiteral("commandsDialog"));
    QMetaObject::invokeMethod(commands, "open");
    QMetaObject::invokeMethod(commands, "requested", Q_ARG(QString, QStringLiteral("newAgent")));
    QMetaObject::invokeMethod(commands, "close");
    QMetaObject::invokeMethod(sign_in, "open");
    fixture.key(QEvent::KeyPress, Qt::Key_X, QStringLiteral("x"));
    lapis::desktop::interaction::paste(nullptr, QStringLiteral("token-value"), true, "paste");
    QMetaObject::invokeMethod(sign_in, "close");
    fixture.key(QEvent::KeyPress, Qt::Key_Y, QStringLiteral("y"));
    const auto lines = fixture.lines();
    const auto commands_run = of_kind(lines, QStringLiteral("command"));
    require(commands_run.size() == 1 && commands_run[0].value("id") == "newAgent",
            "the palette's command is recorded");
    const auto dialogs = of_kind(lines, QStringLiteral("dialog"));
    require(dialogs.size() == 4 && dialogs[0].value("name") == "commandsDialog" &&
                dialogs[0].value("open").toBool() &&
                dialogs[2].value("name") == "planSignInDialog" &&
                !dialogs[3].value("open").toBool() && dialogs[3].contains("unrecordedMs"),
            "dialogs open and close; a credential dialog leaves only a gap");
    const auto keys = of_kind(lines, QStringLiteral("key"));
    require(keys.size() == 1 && keys[0].value("text") == "y" &&
                of_kind(lines, QStringLiteral("paste")).empty(),
            "nothing typed or pasted while the sign-in dialog was open is recorded");
}

void pump(int milliseconds) {
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < milliseconds) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
}

void wait_for(const std::function<bool()>& condition, const std::string& message) {
    QElapsedTimer clock;
    clock.start();
    while (!condition() && clock.elapsed() < 5000)
        pump(10);
    require(condition(), message);
}

QQuickItem* find_item(QQuickItem* root, const QString& name) {
    if (root == nullptr)
        return nullptr;
    if (root->objectName() == name)
        return root;
    for (auto* child : root->childItems())
        if (auto* found = find_item(child, name))
            return found;
    return nullptr;
}

// The production window, offscreen: a click on a strip card names the card's
// agent, and the real sign-in and usage dialogs hold recording.
void production_window_is_described() {
    Fixture fixture;
    fixture.recorder->setSettings({.enabled = true});
    lapis::desktop::UiPreview preview(
        fixture.workspace, {.source = QUrl::fromLocalFile(QStringLiteral(LAPIS_QML_SOURCE)),
                            .compact = true,
                            .screen = QString()});
    require(preview.load(), "production QML loaded");
    auto* window = preview.window();
    window->resize(1280, 900);
    window->show();
    pump(60);
    fixture.recorder->watchWindow(window);
    const auto sessions = fixture.workspace.categorySessions();
    require(!sessions.isEmpty(), "the preview has agents");
    const auto id = sessions.front().value<lapis::desktop::SessionPreview*>()->sessionId();
    auto* card = find_item(window->contentItem(), QStringLiteral("cardPress_") + id);
    require(card != nullptr, "the strip card exists");
    const auto at = card->mapToScene(QPointF(card->width() / 2, card->height() / 2));
    QMouseEvent press(QEvent::MouseButtonPress, at, at, Qt::LeftButton, Qt::LeftButton,
                      Qt::NoModifier);
    QCoreApplication::sendEvent(window, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, at, at, Qt::LeftButton, Qt::NoButton,
                        Qt::NoModifier);
    QCoreApplication::sendEvent(window, &release);
    pump(20);
    for (const auto* name : {"planSignInDialog", "usageDialog"}) {
        auto* dialog = window->findChild<QObject*>(QLatin1String(name));
        require(dialog != nullptr, std::string("the window has ") + name);
        QMetaObject::invokeMethod(dialog, "open");
        wait_for([dialog] { return dialog->property("visible").toBool(); },
                 std::string(name) + " opened");
        fixture.key(QEvent::KeyPress, Qt::Key_Q, QStringLiteral("q"));
        QMetaObject::invokeMethod(dialog, "close");
        wait_for([dialog] { return !dialog->property("visible").toBool(); },
                 std::string(name) + " closed");
    }
    // Every move here is a recorded sample, so each pays the hit test.
    QElapsedTimer sampled;
    sampled.start();
    constexpr int kSamples = 500;
    for (int index = 0; index < kSamples; ++index) {
        QMouseEvent move(QEvent::MouseMove, QPointF(index * 2 % 1280, 400 + index % 400),
                         QPointF(index * 2 % 1280, 400 + index % 400), Qt::NoButton, Qt::NoButton,
                         Qt::NoModifier);
        QCoreApplication::sendEvent(window, &move);
        fixture.now_us += 50'000;
    }
    std::cout << "interaction_log_test: per recorded pointer sample in the production window "
              << static_cast<double>(sampled.nsecsElapsed()) / 1000.0 / kSamples
              << " us (Qt's own hover delivery included)\n";
    const auto lines = fixture.lines();
    const auto mouse = of_kind(lines, QStringLiteral("mouse"));
    require(mouse.size() == 2, "a press and a release");
    const auto target = mouse[0].value("target").toObject();
    require(target.value("area") == "strip" && target.value("session") == id &&
                mouse[0].value("button") == "left",
            "the click names the strip and the card's agent");
    require(of_kind(lines, QStringLiteral("key")).empty(),
            "keys typed with a credential dialog open are not recorded");
    int gaps = 0;
    for (const auto& dialog : of_kind(lines, QStringLiteral("dialog")))
        gaps += dialog.contains("unrecordedMs") ? 1 : 0;
    require(gaps == 2, "each credential dialog leaves one gap record");
}

// Microseconds of GUI-thread time per key event, recording against not.
void measure_cost() {
    Fixture fixture;
    constexpr int kEvents = 20'000;
    const auto run = [&fixture] {
        QElapsedTimer timer;
        timer.start();
        for (int index = 0; index < kEvents / 2; ++index) {
            fixture.key(QEvent::KeyPress, Qt::Key_A, QStringLiteral("a"));
            fixture.key(QEvent::KeyRelease, Qt::Key_A, QStringLiteral("a"));
            QCoreApplication::processEvents();
        }
        return static_cast<double>(timer.nsecsElapsed()) / 1000.0 / kEvents;
    };
    const double off = run();
    fixture.recorder->setSettings({.enabled = true});
    const double on = run();
    fixture.recorder->drain();
    std::cout << "interaction_log_test: per key event " << on << " us recording, " << off
              << " us off, " << on - off << " us added\n";
    QElapsedTimer moves;
    moves.start();
    for (int index = 0; index < kEvents; ++index) {
        fixture.move({static_cast<qreal>(index % 400), 100});
        fixture.now_us += 1'000;
    }
    std::cout << "interaction_log_test: per pointer move "
              << static_cast<double>(moves.nsecsElapsed()) / 1000.0 / kEvents << " us\n";
    // What a key in a focused terminal adds: its cursor row checked for a
    // secret prompt, and (on macOS) the secure input state.
    const QString row = QStringLiteral("> ") + QString(198, u'w');
    QElapsedTimer checks;
    checks.start();
    int hits = 0;
    for (int index = 0; index < kEvents; ++index)
        hits += lapis::desktop::likely_secret_prompt(row) ? 1 : 0;
    std::cout << "interaction_log_test: per secret-prompt check (200 columns) "
              << static_cast<double>(checks.nsecsElapsed()) / 1000.0 / kEvents << " us\n";
    QElapsedTimer secure;
    secure.start();
    for (int index = 0; index < kEvents; ++index)
        hits += lapis::desktop::platform::secure_input_enabled() ? 1 : 0;
    std::cout << "interaction_log_test: per secure input check "
              << static_cast<double>(secure.nsecsElapsed()) / 1000.0 / kEvents << " us\n";
    require(hits >= 0, "checks ran");
    require(on - off < 100, "recording adds well under a frame per event");
}
} // namespace

int main(int argc, char** argv) {
    // Offscreen: the recorder sees synthetic events; no window is shown.
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_QUICK_CONTROLS_STYLE", "Basic");
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
    QGuiApplication app(argc, argv);
    qmlRegisterUncreatableType<lapis::desktop::SessionPreview>("Lapis", 1, 0, "SessionPreview",
                                                               "owned by the workspace");
    qmlRegisterType<lapis::desktop::TerminalSurface>("Lapis", 1, 0, "TerminalSurface");
    qmlRegisterUncreatableType<lapis::desktop::KeyMap>("Lapis", 1, 0, "KeyMap",
                                                       "owned by the application");
    try {
        settings_parse_with_defaults_and_limits();
        secret_prompts_are_recognized();
        pointer_moves_are_sampled_and_coalesced();
        writer_formats_rotates_and_stays_private();
        writer_refuses_a_shared_folder();
        disabled_writes_nothing();
        records_keys_ime_and_paste();
        secret_input_is_withheld();
        pointer_and_wheel_are_coalesced();
        credential_dialogs_pause_recording();
        production_window_is_described();
        measure_cost();
    } catch (const std::exception& error) {
        std::cerr << "interaction_log_test: " << error.what() << '\n';
        return 1;
    }
    std::cout << "interaction_log_test: PASS\n";
    return 0;
}
