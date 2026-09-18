#include "platform/native_input_driver.hpp"
#include "terminal_surface.hpp"
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
#include <QLocalSocket>
#include <QMimeData>
#include <QProcess>
#include <QProcessEnvironment>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>

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
void until(const std::function<bool()>& condition) {
    QElapsedTimer timer;
    timer.start();
    while (!condition()) {
        require(timer.elapsed() < 10000, "Native event deadline expired");
        pump(5);
    }
}
struct ClipboardBackup {
    std::unique_ptr<QMimeData> saved{std::make_unique<QMimeData>()};
    ClipboardBackup() {
        const auto* current = QGuiApplication::clipboard()->mimeData();
        if (current)
            for (const auto& type : current->formats())
                saved->setData(type, current->data(type));
    }
    ~ClipboardBackup() { QGuiApplication::clipboard()->setMimeData(saved.release()); }
};
struct Observations final : QObject {
    QStringList preedits, commits;
    bool eventFilter(QObject*, QEvent* event) override {
        if (event->type() == QEvent::InputMethod) {
            const auto* input = static_cast<QInputMethodEvent*>(event);
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
    ClipboardBackup clipboard;
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
    void expect(const QString& name, const QByteArray& expected,
                const std::function<void()>& action) {
        std::cerr << "Checking " << name.toStdString() << "\n";
        const auto before = fixture.bytes();
        action();
        until([&] { return fixture.bytes().size() >= before.size() + expected.size(); });
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
                   key(0);
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
    void run() {
        expect(QStringLiteral("native printable and Return"), QByteArray("a\r"), [&] {
            key(0);
            key(36);
        });
        expect(QStringLiteral("native Control-C"), QByteArray(1, '\x03'),
               [&] { key(8, NativeModifiers::control); });
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
        const auto count = observations.preedits.size();
        expect(QStringLiteral("real Japanese IME preedit emits no PTY bytes"), {}, [&] { key(0); });
        require(observations.preedits.size() > count, "Native IME produced no preedit event");
        expect(QStringLiteral("real Japanese IME commit"), QStringLiteral("あ").toUtf8(),
               [&] { key(36); });
        expect(QStringLiteral("real Japanese IME cancellation"), {}, [&] {
            key(40);
            key(0);
            key(53);
            key(53);
        });
        expect(QStringLiteral("IME recovers after cancellation"), QStringLiteral("あ").toUtf8(),
               [&] {
                   key(0);
                   key(36);
               });
        require(!observations.commits.isEmpty(), "Native IME produced no commit event");
        expect(QStringLiteral("IME composition cleared by history mode"), {}, [&] {
            key(0);
            document.olderHistory();
            pump(200);
            document.returnToLive();
            surface.forceActiveFocus();
            pump(100);
        });
        expect(QStringLiteral("IME recovers after history return"), QStringLiteral("あ").toUtf8(),
               [&] {
                   key(0);
                   key(36);
               });
        expect(QStringLiteral("IME composition cleared by document detach"), {}, [&] {
            key(0);
            surface.setDocument(nullptr);
            pump(100);
            surface.setDocument(&document);
            surface.forceActiveFocus();
            pump(100);
        });
        expect(QStringLiteral("IME recovers after document reattach"),
               QStringLiteral("あ").toUtf8(), [&] {
                   key(0);
                   key(36);
               });
        focus_transition();
        expect(QStringLiteral("IME recovers after native focus return"),
               QStringLiteral("あ").toUtf8(), [&] {
                   key(0);
                   key(36);
               });
        expect(QStringLiteral("IME composition cleared by attachment replacement and reconnect"),
               {}, [&] {
                   key(0);
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
                   key(0);
                   key(36);
               });
        expect(QStringLiteral("IME composition survives resize"), QStringLiteral("あ").toUtf8(),
               [&] {
                   key(0);
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
        {
            Qualification qualification;
            qualification.run();
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
        receipt.insert(QStringLiteral("input_path"),
                       QStringLiteral("CoreGraphics keyboard events through AppKit, Apple Japanese "
                                      "IME, Qt and the real PTY service"));
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
