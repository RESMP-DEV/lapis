#include "clipboard_backup.hpp"
#include "platform/native_input_driver.hpp"
#include "terminal_surface.hpp"
#include "workspace_supervisor.hpp"
#include <QClipboard>
#include <QCommandLineParser>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QInputMethod>
#include <QInputMethodEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLocalSocket>
#include <QMimeData>
#include <QProcess>
#include <QProcessEnvironment>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <source_location>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using lapis::desktop::SessionPreview;
using lapis::desktop::TerminalSurface;
using lapis::desktop::test::NativeInputDriver;
using lapis::desktop::test::NativeModifiers;
namespace session = lapis::session;
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
void pump(int milliseconds) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < milliseconds) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
}
void until(const std::function<bool()>& condition,
           const std::source_location where = std::source_location::current()) {
    QElapsedTimer timer;
    timer.start();
    while (!condition()) {
        if (timer.elapsed() >= 10000)
            throw std::runtime_error("Native event deadline expired at line " +
                                     std::to_string(where.line()));
        pump(5);
    }
}

struct Observations final : QObject {
    QStringList preedits, commits;
    bool composing{false};
    bool eventFilter(QObject*, QEvent* event) override {
        if (qEnvironmentVariableIsSet("LAPIS_NATIVE_TRACE") &&
            (event->type() == QEvent::KeyPress || event->type() == QEvent::KeyRelease)) {
            const auto* key = static_cast<QKeyEvent*>(event);
            std::cerr << "Qt key type=" << event->type() << " key=" << key->key()
                      << " mods=" << key->modifiers().toInt()
                      << " text=" << key->text().toUtf8().toHex().toStdString() << '\n';
        }
        if (event->type() == QEvent::InputMethod) {
            const auto* input = static_cast<QInputMethodEvent*>(event);
            composing = !input->preeditString().isEmpty();
            if (qEnvironmentVariableIsSet("LAPIS_NATIVE_TRACE"))
                std::cerr << "Qt IME preedit="
                          << input->preeditString().toUtf8().toHex().toStdString()
                          << " commit=" << input->commitString().toUtf8().toHex().toStdString()
                          << '\n';
            if (!input->preeditString().isEmpty())
                preedits.append(input->preeditString());
            if (!input->commitString().isEmpty())
                commits.append(input->commitString());
        }
        return false;
    }
};
struct Fixture {
    QTemporaryDir directory{QStringLiteral("/tmp/lapis-native-XXXXXX")};
    QProcess process;
    session::LaunchSpec launch;
    QString endpoint;
    QString receipt;
    Fixture() {
        require(directory.isValid(), "Native fixture directory failed");
        endpoint = directory.filePath(QStringLiteral("session.sock"));
        receipt = directory.filePath(QStringLiteral("received.bin"));
        const QString script =
            QStringLiteral("import os,sys,tty\n"
                           "f=open(sys.argv[1],'wb',buffering=0);tty.setraw(0)\n"
                           "os.write(1,b'\\x1b[?2004hNATIVE_READY\\r\\n')\n"
                           "while True:\n"
                           " data=os.read(0,65536)\n"
                           " if not data:break\n"
                           " f.write(data)\n"
                           " os.write(1,b'RECEIVED '+data.hex().encode()+b'\\r\\n')\n");
        launch = session::validate_launch(
            {.program = QStandardPaths::findExecutable(QStringLiteral("python3")),
             .arguments = {QStringLiteral("-u"), QStringLiteral("-c"), script, receipt},
             .directory = directory.path()});
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("LAPIS_HISTORY_ROOT"),
                           directory.filePath(QStringLiteral("history")));
        process.setProcessEnvironment(environment);
        process.setStandardOutputFile(directory.filePath(QStringLiteral("service.log")));
        process.setStandardErrorFile(directory.filePath(QStringLiteral("service.log")),
                                     QIODevice::Append);
        QStringList arguments{endpoint, launch.directory, launch.program};
        arguments.append(launch.arguments);
        process.start(QStringLiteral(LAPIS_SESSION_SERVICE_PATH), arguments);
        require(process.waitForStarted(3000), "Native fixture service launch failed");
        until([&] { return QFile::exists(endpoint); });
    }
    ~Fixture() {
        process.terminate();
        if (!process.waitForFinished(3000)) {
            process.kill();
            process.waitForFinished(3000);
        }
    }
    [[nodiscard]] QByteArray bytes() const {
        QFile file(receipt);
        require(file.open(QIODevice::ReadOnly), "Could not read controlled PTY receipt");
        return file.readAll();
    }
};
struct Qualification {
    Fixture fixture;
    SessionPreview document{QStringLiteral("native input qualification"),
                            fixture.directory.path(),
                            {},
                            QColor(Qt::white),
                            ""};
    QQuickWindow window;
    TerminalSurface surface{window.contentItem()};
    NativeInputDriver driver;
    lapis::desktop::test::ClipboardBackup clipboard;
    Observations observations;
    QJsonArray cases;
    Qualification() {
        window.setTitle(QStringLiteral("lapis automated native input qualification"));
        window.setGeometry(100, 100, 900, 540);
        surface.setSize(QSizeF(900, 540));
        surface.setDocument(&document);
        surface.setInteractive(true);
        surface.installEventFilter(&observations);
        document.startLive(fixture.endpoint, fixture.launch, session::wire::AttachMode::discover);
        window.show();
        until([&] { return window.isExposed(); });
        driver.activate(window);
        surface.forceActiveFocus();
        until([&] {
            return document.inputReady() && surface.inputMethodQuery(Qt::ImEnabled).toBool() &&
                   QFile::exists(fixture.receipt);
        });
        driver.selectUS();
        pump(200);
        require(driver.selectedSource() == QStringLiteral("com.apple.keylayout.US"),
                "US source not selected");
    }
    void key(std::uint16_t code, NativeModifiers flags = NativeModifiers::none) {
        driver.key(code, flags);
        pump(100);
    }
    void preeditKey(std::uint16_t code = 0) {
        const auto count = observations.preedits.size();
        key(code);
        until([&] { return observations.preedits.size() > count && observations.composing; });
    }
    void expect(const QString& name, const QByteArray& expected,
                const std::function<void()>& action) {
        std::cerr << "Checking " << name.toStdString() << "\n";
        // Each case begins with this fixture owning input; actions may then
        // deliberately transfer ownership to test cancellation and recovery.
        driver.activate(window);
        surface.forceActiveFocus();
        try {
            until([&] {
                return window.isActive() && surface.inputMethodQuery(Qt::ImEnabled).toBool();
            });
        } catch (const std::exception&) {
            throw std::runtime_error(name.toStdString() + ": fixture activation deadline; active=" +
                                     std::to_string(window.isActive()) +
                                     " focused=" + std::to_string(surface.hasActiveFocus()) +
                                     " ready=" + std::to_string(document.inputReady()));
        }
        const auto before = fixture.bytes();
        action();
        try {
            until([&] { return fixture.bytes().size() >= before.size() + expected.size(); });
        } catch (const std::exception&) {
            throw std::runtime_error(name.toStdString() + ": input deadline; active=" +
                                     std::to_string(window.isActive()) +
                                     " focused=" + std::to_string(surface.hasActiveFocus()) +
                                     " ready=" + std::to_string(document.inputReady()));
        }
        pump(100);
        const auto actual = fixture.bytes().sliced(before.size());
        if (actual != expected)
            throw std::runtime_error(name.toStdString() + ": expected " +
                                     expected.toHex().toStdString() + ", got " +
                                     actual.toHex().toStdString());
        cases.append(
            QJsonObject{{QStringLiteral("case"), name},
                        {QStringLiteral("received_hex"), QString::fromLatin1(actual.toHex())},
                        {QStringLiteral("passed"), true}});
    }
    void japanese() {
        driver.selectJapanese();
        pump(200);
        require(driver.selectedSource() ==
                    QStringLiteral("com.apple.inputmethod.Kotoeri.RomajiTyping.Japanese"),
                "Japanese source not selected");
    }
    void focus_transition() {
        Fixture other_fixture;
        SessionPreview other_document(QStringLiteral("focus destination"),
                                      other_fixture.directory.path(), {}, QColor(Qt::white), "");
        QQuickWindow other;
        other.setTitle(QStringLiteral("lapis native focus destination"));
        other.setGeometry(150, 150, 600, 320);
        TerminalSurface other_surface(other.contentItem());
        other_surface.setSize(QSizeF(600, 320));
        other_surface.setDocument(&other_document);
        other_surface.setInteractive(true);
        other_document.startLive(other_fixture.endpoint, other_fixture.launch,
                                 session::wire::AttachMode::discover);
        other.show();
        until([&] {
            return other.isExposed() && other_document.inputReady() &&
                   QFile::exists(other_fixture.receipt);
        });
        driver.activate(window);
        surface.forceActiveFocus();
        until([&] { return surface.inputMethodQuery(Qt::ImEnabled).toBool(); });
        japanese();
        expect(QStringLiteral("native IME focus commit stays with its original terminal"),
               QStringLiteral("あ").toUtf8(), [&] {
                   preeditKey();
                   driver.activate(other);
                   other_surface.forceActiveFocus();
                   until([&] { return other.isActive(); });
                   pump(150);
                   require(other_fixture.bytes().isEmpty(),
                           "Focus change leaked composition into the new terminal");
                   driver.activate(window);
                   surface.forceActiveFocus();
                   until([&] { return surface.inputMethodQuery(Qt::ImEnabled).toBool(); });
                   pump(100);
               });
        require(other_fixture.bytes().isEmpty(),
                "Returning focus leaked input into the other terminal");
    }
    void append_case(const QString& name, const QByteArray& first, const QByteArray& second,
                     int index_before, int index_after_commit) {
        cases.append(QJsonObject{
            {QStringLiteral("case"), name},
            {QStringLiteral("first_received_hex"), QString::fromLatin1(first.toHex())},
            {QStringLiteral("second_received_hex"), QString::fromLatin1(second.toHex())},
            {QStringLiteral("workspace_index_before"), index_before},
            {QStringLiteral("workspace_index_after_commit"), index_after_commit},
            {QStringLiteral("passed"), true}});
    }
    void workspace_switch() {
        QTemporaryDir workspace_directory{QStringLiteral("/tmp/lapis-native-workspace-XXXXXX")};
        require(workspace_directory.isValid(), "Workspace fixture directory failed");
        const QString manifest = workspace_directory.filePath(QStringLiteral("workspace.json"));
        Fixture first_fixture;
        Fixture second_fixture;
        std::vector<lapis::desktop::WorkspaceEntry> entries;
        {
            SessionPreview first_document(QStringLiteral("workspace source"),
                                          first_fixture.directory.path(), {}, QColor(Qt::white),
                                          "");
            SessionPreview second_document(QStringLiteral("workspace destination"),
                                           second_fixture.directory.path(), {}, QColor(Qt::white),
                                           "");
            first_document.startLive(first_fixture.endpoint, first_fixture.launch,
                                     session::wire::AttachMode::discover);
            second_document.startLive(second_fixture.endpoint, second_fixture.launch,
                                      session::wire::AttachMode::discover);
            until([&] {
                return first_document.inputReady() && second_document.inputReady() &&
                       first_document.reconnectEntry() && second_document.reconnectEntry();
            });
            const auto first_entry = first_document.reconnectEntry();
            const auto second_entry = second_document.reconnectEntry();
            if (!first_entry || !second_entry)
                throw std::runtime_error("Workspace fixture lost verified reconnect metadata");
            entries.push_back(*first_entry);
            entries.push_back(*second_entry);
            lapis::desktop::WorkspaceRegistry registry{manifest};
            registry.write(entries);
        }

        lapis::desktop::WorkspaceOptions options;
        options.manifest = manifest;
        lapis::desktop::Workspace workspace{lapis::desktop::WorkspaceMode::live, options};
        qint64 injected_now = 1000000;
        lapis::desktop::WorkspaceSupervisor supervisor{workspace,
                                                       [&injected_now] { return injected_now; }};
        const auto focus_connection =
            QObject::connect(&workspace, &lapis::desktop::Workspace::focusChanged, &surface,
                             [&] { surface.setDocument(workspace.focusedSession()); });
        until([&] {
            return !workspace.loading() && workspace.sessions().size() == 2 &&
                   workspace.focusedSession() && workspace.focusedSession()->inputReady();
        });
        until([&] { return workspace.sessions().back().value<SessionPreview*>()->inputReady(); });
        surface.setDocument(workspace.focusedSession());
        surface.setFocusWorkspace(&workspace);
        driver.activate(window);
        surface.forceActiveFocus();
        until([&] {
            return window.isActive() && surface.hasActiveFocus() &&
                   surface.inputMethodQuery(Qt::ImEnabled).toBool();
        });
        supervisor.setWindowActive(window.isActive());
        japanese();
        const QByteArray first_before = first_fixture.bytes();
        const QByteArray second_before = second_fixture.bytes();
        preeditKey();
        supervisor.setEnabled(true);
        injected_now += 5001;
        supervisor.tick();
        require(workspace.focusedIndex() == 0,
                "Automatic supervisor switched during native IME composition");
        workspace.setFocusedIndex(1);
        require(workspace.focusedIndex() == 0,
                "Active IME composition switched workspace focus immediately");
        key(36);
        until([&] { return workspace.focusedIndex() == 1; });
        require(surface.document() == workspace.focusedSession(),
                "Terminal document did not follow committed workspace focus");
        pump(100);
        const QByteArray first_commit = first_fixture.bytes().sliced(first_before.size());
        const QByteArray second_during_ime = second_fixture.bytes().sliced(second_before.size());
        require(first_commit == QStringLiteral("あ").toUtf8(),
                "IME commit did not reach the original workspace session");
        require(second_during_ime.isEmpty(), "IME switch leaked input into destination session");
        append_case(QStringLiteral("workspace IME commit defers focus switch"), first_commit,
                    second_during_ime, 0, 1);

        driver.selectUS();
        pump(200);
        const QByteArray first_after_ime = first_fixture.bytes();
        const QByteArray second_after_ime = second_fixture.bytes();
        key(11);
        const QByteArray first_after_b = first_fixture.bytes().sliced(first_after_ime.size());
        const QByteArray second_after_b = second_fixture.bytes().sliced(second_after_ime.size());
        require(first_after_b.isEmpty(), "US input reached the unfocused workspace session");
        require(second_after_b == QByteArray("b"),
                "US input did not reach the focused workspace session");
        append_case(QStringLiteral("workspace US input follows focus"), first_after_b,
                    second_after_b, 1, 1);

        workspace.setFocusedIndex(0);
        until([&] { return workspace.focusedIndex() == 0; });
        const QString split_paste = QStringLiteral("native-paste-界\nsecond-half");
        QGuiApplication::clipboard()->setText(split_paste);
        const QByteArray bracketed_split_paste =
            QByteArray("\x1b[200~") + split_paste.toUtf8() + QByteArray("\x1b[201~");
        const QByteArray first_paste_before = first_fixture.bytes();
        const QByteArray second_paste_before = second_fixture.bytes();
        supervisor.setEnabled(true);
        bool paste_block_observed = false;
        bool paste_owner_preserved = false;
        QObject paste_observation;
        const auto paste_connection = QObject::connect(
            &workspace, &lapis::desktop::Workspace::interactionChanged, &paste_observation, [&] {
                if (!workspace.interactionBlocked() || paste_block_observed)
                    return;
                paste_block_observed = true;
                injected_now += 5001;
                supervisor.tick();
                paste_owner_preserved = workspace.focusedIndex() == 0;
                supervisor.setEnabled(false);
            });
        key(9, NativeModifiers::command);
        QObject::disconnect(paste_connection);
        require(paste_block_observed, "Native paste interaction block was not observed");
        require(paste_owner_preserved,
                "Automatic supervisor switched while native paste was active");
        until([&] {
            return first_fixture.bytes().size() >=
                   first_paste_before.size() + bracketed_split_paste.size();
        });
        const QByteArray first_paste = first_fixture.bytes().sliced(first_paste_before.size());
        const QByteArray second_paste = second_fixture.bytes().sliced(second_paste_before.size());
        require(first_paste == bracketed_split_paste,
                "Native paste did not remain one bracketed input");
        require(second_paste.isEmpty(), "Automatic navigation split paste into the destination");
        append_case(QStringLiteral("workspace supervisor cannot split native paste"), first_paste,
                    second_paste, 0, 0);

        supervisor.setEnabled(false);
        supervisor.setWindowActive(false);
        QObject::disconnect(focus_connection);
        surface.setFocusWorkspace(nullptr);
        surface.setDocument(&document);
    }
    void run() {
        expect(QStringLiteral("native printable and Return"), QByteArray("a\r"), [&] {
            key(0);
            key(36);
        });
        expect(QStringLiteral("native Control-C"), QByteArray(1, '\x03'),
               [&] { key(8, NativeModifiers::control); });
        expect(QStringLiteral("native Command-Left sends Control-A"), QByteArray(1, '\x01'),
               [&] { key(123, NativeModifiers::command); });
        expect(QStringLiteral("native Command-Right sends Control-E"), QByteArray(1, '\x05'),
               [&] { key(124, NativeModifiers::command); });
        expect(QStringLiteral("native Option-B"),
               QByteArray("\x1b"
                          "b"),
               [&] { key(11, NativeModifiers::option); });
        const QString paste = QStringLiteral("paste界\nsecond");
        QGuiApplication::clipboard()->setText(paste);
        expect(QStringLiteral("native Command-V bracketed paste"),
               QByteArray("\x1b[200~") + paste.toUtf8() + QByteArray("\x1b[201~"),
               [&] { key(9, NativeModifiers::command); });
        japanese();
        const auto before_preedit = fixture.bytes();
        expect(QStringLiteral("real Japanese IME preedit emits no PTY bytes"), {},
               [&] { preeditKey(); });
        require(fixture.bytes() == before_preedit, "Native preedit leaked delayed PTY bytes");
        expect(QStringLiteral("real Japanese IME commit"), QStringLiteral("あ").toUtf8(),
               [&] { key(36); });
        expect(QStringLiteral("real Japanese IME cancellation"), {}, [&] {
            preeditKey(40);
            preeditKey();
            key(53);
            key(53);
            until([&] { return !observations.composing; });
        });
        expect(QStringLiteral("IME recovers after cancellation"), QStringLiteral("あ").toUtf8(),
               [&] {
                   preeditKey();
                   key(36);
               });
        require(!observations.commits.isEmpty(), "Native IME produced no commit event");
        expect(QStringLiteral("IME composition cleared by history mode"), {}, [&] {
            preeditKey();
            document.olderHistory();
            pump(200);
            document.returnToLive();
            surface.forceActiveFocus();
            pump(100);
        });
        expect(QStringLiteral("IME recovers after history return"), QStringLiteral("あ").toUtf8(),
               [&] {
                   preeditKey();
                   key(36);
               });
        expect(QStringLiteral("IME composition cleared by document detach"), {}, [&] {
            preeditKey();
            surface.setDocument(nullptr);
            pump(100);
            surface.setDocument(&document);
            surface.forceActiveFocus();
            pump(100);
        });
        expect(QStringLiteral("IME recovers after document reattach"),
               QStringLiteral("あ").toUtf8(), [&] {
                   preeditKey();
                   key(36);
               });
        focus_transition();
        expect(QStringLiteral("IME recovers after native focus return"),
               QStringLiteral("あ").toUtf8(), [&] {
                   preeditKey();
                   key(36);
               });
        expect(QStringLiteral("IME composition cleared by attachment replacement and reconnect"),
               {}, [&] {
                   preeditKey();
                   const auto identity = document.serviceSessionId();
                   QLocalSocket replacement;
                   replacement.connectToServer(fixture.endpoint);
                   require(replacement.waitForConnected(3000),
                           "Replacement client connection failed");
                   const auto attach = session::wire::frame(
                       session::wire::Kind::attach,
                       session::wire::encode_attach(
                           {.mode = session::wire::AttachMode::discover,
                            .fingerprint = session::launch_fingerprint(fixture.launch),
                            .expected = {}}));
                   require(replacement.write(attach) == attach.size(),
                           "Replacement attachment write failed");
                   replacement.flush();
                   until([&] { return !document.inputReady(); });
                   require(document.connectionState() == QStringLiteral("replaced"),
                           "Attachment did not actually replace the connection");
                   replacement.abort();
                   document.reconnect();
                   until([&] { return document.inputReady(); });
                   require(document.serviceSessionId() == identity,
                           "Reconnect changed session identity");
                   surface.forceActiveFocus();
                   pump(100);
               });
        expect(QStringLiteral("IME recovers after service reconnect"),
               QStringLiteral("あ").toUtf8(), [&] {
                   preeditKey();
                   key(36);
               });
        expect(QStringLiteral("IME composition survives resize"), QStringLiteral("あ").toUtf8(),
               [&] {
                   preeditKey();
                   surface.setSize(QSizeF(760, 460));
                   window.resize(760, 460);
                   pump(150);
                   const auto candidate = QGuiApplication::inputMethod()->cursorRectangle();
                   require(!candidate.isEmpty() && surface.boundingRect().intersects(candidate),
                           "Native candidate anchor left terminal bounds");
                   key(36);
               });
        driver.selectUS();
        pump(100);
        expect(QStringLiteral("US keyboard restored"), QByteArray("b"), [&] { key(11); });
        workspace_switch();
    }
};
} // namespace
int main(int argc, char** argv) {
    qputenv("QT_MTL_NO_TRANSACTION", "1");
    qputenv("QT_VULKAN_LIB", LAPIS_VULKAN_LIBRARY);
    QCoreApplication::setAttribute(Qt::AA_MacDontSwapCtrlAndMeta);
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Vulkan);
    QGuiApplication app(argc, argv);
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption(
        {QStringLiteral("output"), QStringLiteral("JSON receipt"), QStringLiteral("path")});
    parser.process(app);
    QJsonObject receipt{{QStringLiteral("schema"), QStringLiteral("lapis.native-input/1")},
                        {QStringLiteral("passed"), false}};
    int result = 1;
    try {
        const auto original_source = NativeInputDriver::selectedSource();
        const auto original_enabled = NativeInputDriver::enabledSources();
        QMap<QString, QByteArray> original_clipboard;
        if (const auto* mime = QGuiApplication::clipboard()->mimeData())
            for (const auto& format : mime->formats())
                original_clipboard.insert(format, mime->data(format));
        std::exception_ptr failure;
        {
            Qualification qualification;
            try {
                qualification.run();
            } catch (...) {
                failure = std::current_exception();
            }
            receipt.insert(QStringLiteral("cases"), qualification.cases);
            receipt.insert(QStringLiteral("preedit_events"),
                           qualification.observations.preedits.size());
            receipt.insert(QStringLiteral("commit_events"),
                           qualification.observations.commits.size());
        }
        pump(100);
        require(NativeInputDriver::selectedSource() == original_source,
                "Original input source was not restored");
        require(NativeInputDriver::enabledSources() == original_enabled,
                "Enabled input source inventory was not restored");
        const auto* restored_clipboard = QGuiApplication::clipboard()->mimeData();
        for (auto entry = original_clipboard.cbegin(); entry != original_clipboard.cend(); ++entry)
            require(restored_clipboard && restored_clipboard->data(entry.key()) == entry.value(),
                    "Original clipboard format was not restored");
        receipt.insert(QStringLiteral("input_source_restored"), true);
        receipt.insert(QStringLiteral("enabled_sources_restored"), true);
        receipt.insert(QStringLiteral("clipboard_restored"), true);
        receipt.insert(
            QStringLiteral("workspace_supervisor"),
            QJsonObject{{"clock", QStringLiteral("injected milliseconds")},
                        {"window_activity", QStringLiteral("mirrored from active window")},
                        {"planned_guard_cases", 2},
                        {"timing_gate", false}});
        receipt.insert(QStringLiteral("input_path"),
                       QStringLiteral("CoreGraphics keyboard events through AppKit, Apple Japanese "
                                      "IME, Qt and the real PTY service"));
        if (failure)
            std::rethrow_exception(failure);
        receipt.insert(QStringLiteral("passed"), true);
        result = 0;
    } catch (const std::exception& error) {
        receipt.insert(QStringLiteral("error"), QString::fromUtf8(error.what()));
        std::cerr << error.what() << '\n';
    }
    QFile output(parser.value(QStringLiteral("output")));
    if (!output.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return 2;
    const auto bytes = QJsonDocument(receipt).toJson();
    if (output.write(bytes) != bytes.size())
        return 2;
    return result;
}
