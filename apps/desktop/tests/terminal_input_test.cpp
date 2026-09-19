#include "session_descriptor.hpp"
#include "transport/local_protocol.hpp"
#include "workspace.hpp"

#include "platform/window_activation.hpp"
#include "terminal_surface.hpp"
#include <QClipboard>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QGuiApplication>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QLocalServer>
#include <QLocalSocket>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QTemporaryDir>
#include <QThread>
#include <functional>
#include <iostream>
#include <source_location>
#include <stdexcept>

namespace {
namespace wire = lapis::session::wire;
using lapis::desktop::SessionPreview;
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
void until(const std::function<bool()>& condition,
           std::source_location where = std::source_location::current()) {
    QElapsedTimer time;
    time.start();
    while (time.elapsed() < 8000) {
        if (condition())
            return;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
    throw std::runtime_error("Event deadline expired at line " + std::to_string(where.line()));
}
void settle() {
    QElapsedTimer time;
    time.start();
    while (time.elapsed() < 50) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
}
struct Peer {
    QLocalSocket* socket{}; // QLocalServer owns accepted sockets.
    QByteArray bytes;
    wire::Frame read() {
        wire::Frame frame;
        bool received = false;
        until([&] {
            bytes += socket->readAll();
            received = wire::take_frame(bytes, frame);
            return received;
        });
        return frame;
    }
    void send(wire::Kind kind, const QByteArray& payload) {
        const auto encoded = wire::frame(kind, payload);
        require(socket->write(encoded) == encoded.size(), "Could not send fixture frame");
        socket->flush();
    }
};
struct Fixture {
    QLocalServer server;
    QTemporaryDir directory{QStringLiteral("/tmp/lapis-v4-XXXXXX")};
    lapis::session::LaunchSpec launch;
    wire::SessionIdentity identity{wire::new_id(), wire::new_id()};
    lapis::session::Terminal terminal{{4, 2}};
    QString endpoint;
    quint64 generation_{1};
    SessionPreview document{
        QStringLiteral("test"), QStringLiteral("/tmp"), {}, QColor(Qt::white), ""};
    Fixture() {
        require(directory.isValid(), "Temporary directory failed");
        endpoint = QDir(directory.path()).absoluteFilePath(QStringLiteral("session.sock"));
        launch = lapis::session::validate_launch({.program = QStringLiteral("/bin/cat"),
                                                  .arguments = {},
                                                  .directory = directory.path()});
        server.setSocketOptions(QLocalServer::UserAccessOption);
        require(server.listen(endpoint), "Fixture listener failed");
        terminal.feed("screen");
    }
    ~Fixture() { server.close(); }
    Peer accept() {
        until([&] { return server.hasPendingConnections(); });
        return {server.nextPendingConnection(), {}};
    }
    wire::AttachRequest request(Peer& peer) {
        const auto frame = peer.read();
        require(frame.kind == wire::Kind::attach, "Expected attachment request");
        const auto request = wire::decode_attach(frame.payload);
        require(request.fingerprint == lapis::session::launch_fingerprint(launch),
                "Wrong launch fingerprint");
        return request;
    }
    void hello(Peer& peer, quint64 generation = 1) {
        generation_ = generation;
        peer.send(wire::Kind::hello, wire::encode_hello({{identity, generation}, 123}));
        until([&] { return document.connectionState() == QStringLiteral("synchronizing"); });
        require(!document.inputReady(), "Hello enabled input before the screen");
    }
    void screen(Peer& peer, quint64 generation = 1, quint64 sequence = 1) {
        peer.send(
            wire::Kind::snapshot,
            wire::encode_snapshot_message({{identity, generation}, sequence, terminal.snapshot()}));
        until([&] { return document.connectionState() == QStringLiteral("ready"); });
        const auto ack = peer.read();
        require(ack.kind == wire::Kind::ready, "Screen was not acknowledged before input");
        const auto ready = wire::decode_ready(ack.payload);
        require(ready.attachment == wire::Attachment{identity, generation} &&
                    ready.sequence == sequence,
                "Invalid synchronization acknowledgement");
        const auto resize = peer.read();
        require(resize.kind == wire::Kind::resize, "Expected post-synchronization resize");
    }
    wire::HistoryRequest historyRequest(Peer& peer) {
        const auto frame = peer.read();
        require(frame.kind == wire::Kind::history_request, "Expected history request");
        const auto control = wire::decode_control(frame.payload);
        require(control.attachment == wire::Attachment{identity, generation_},
                "History request lacked current attachment");
        return wire::decode_history_request(control.payload);
    }
    void historyReply(Peer& peer, quint64 request_id, quint64 page_id,
                      const lapis::session::TerminalSnapshot& snapshot = {},
                      const QString& message = {}, const wire::Attachment& attachment = {}) {
        peer.send(wire::Kind::history_page,
                  wire::encode_history_reply(
                      {attachment == wire::Attachment{} ? wire::Attachment{identity, generation_}
                                                        : attachment,
                       request_id, page_id, message,
                       page_id == 0 ? std::nullopt : std::optional(snapshot)}));
    }
};

QByteArray text_frames(Peer& peer, qsizetype minimum = 0) {
    if (minimum == 0)
        settle();
    QByteArray text;
    until([&] {
        peer.bytes += peer.socket->readAll();
        wire::Frame frame;
        while (wire::take_frame(peer.bytes, frame)) {
            if (frame.kind == wire::Kind::text || frame.kind == wire::Kind::paste)
                text += wire::decode_control(frame.payload).payload;
            else
                require(frame.kind == wire::Kind::resize ||
                            frame.kind == wire::Kind::history_request,
                        "Unexpected input frame");
        }
        return text.size() >= minimum;
    });
    return text;
}
void composition(lapis::desktop::TerminalSurface& surface, QStringView preedit,
                 const QString& commit = {}, int replace = 0) {
    QInputMethodEvent event(preedit.toString(), {});
    event.setCommitString(commit, replace, replace == 0 ? 0 : 1);
    QCoreApplication::sendEvent(&surface, &event);
}
void input_contract() {
    Fixture f;
    QQuickWindow window;
    window.setGeometry(100, 100, 640, 360);
    lapis::desktop::TerminalSurface surface(window.contentItem());
    surface.setSize(QSizeF(640, 360));
    surface.setDocument(&f.document);
    surface.setInteractive(true);
    f.document.startLive(f.endpoint, f.launch, wire::AttachMode::discover);
    auto peer = f.accept();
    static_cast<void>(f.request(peer));
    f.hello(peer);
    f.screen(peer);
    window.show();
    until([&] { return window.isExposed(); });
    lapis::desktop::test::activate_test_window(window);
    settle(); // Drain native activation events before beginning an IME transaction.
    surface.forceActiveFocus();
    until([&] { return window.isActive() && surface.hasActiveFocus(); });
    static_cast<void>(text_frames(peer));
    require(surface.inputMethodQuery(Qt::ImEnabled).toBool(), "Ready terminal disabled IME");
    const auto original = surface.inputMethodQuery(Qt::ImCursorRectangle).toRectF();
    require(!original.isEmpty(), "IME candidate rectangle missing");
    composition(surface, {}, QStringLiteral("✓"));
    require(text_frames(peer, 3) == QStringLiteral("✓").toUtf8(),
            "Idle terminal rejected commit-only input");
    composition(surface, {}, QStringLiteral("★"));
    require(text_frames(peer, 3) == QStringLiteral("★").toUtf8(),
            "Repeated commit-only input was rejected");
    QKeyEvent printable(QEvent::KeyPress, Qt::Key_X, Qt::NoModifier, QStringLiteral("x"));
    QCoreApplication::sendEvent(&surface, &printable);
    require(text_frames(peer, 1) == QByteArray("x"), "Printable key fixture did not reach PTY");
    {
        QQuickItem other_focus(window.contentItem());
        other_focus.forceActiveFocus();
        lapis::desktop::test::activate_test_window(window);
        settle(); // Drain native activation events before beginning an IME transaction.
        surface.forceActiveFocus();
        until([&] { return surface.inputMethodQuery(Qt::ImEnabled).toBool(); });
        composition(surface, {}, QStringLiteral("✓"));
        require(text_frames(peer, 3) == QStringLiteral("✓").toUtf8(),
                "Ordinary typing and focus return incorrectly invalidated commit-only input");
    }
    composition(surface, QStringLiteral("にほん"));
    require(text_frames(peer).isEmpty(), "Preedit leaked into PTY");
    composition(surface, {}, QStringLiteral("日本"));
    require(text_frames(peer, 6) == QStringLiteral("日本").toUtf8(), "IME commit changed bytes");
    composition(surface, QStringLiteral("pending"));
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QCoreApplication::sendEvent(&surface, &escape);
    composition(surface, {}, QStringLiteral("late"));
    require(text_frames(peer).isEmpty(), "Canceled composition committed late");
    composition(surface, QStringLiteral("cancel-natively"));
    composition(surface, {});
    composition(surface, {}, QStringLiteral("late-native-cancel"));
    require(text_frames(peer).isEmpty(), "Empty preedit cancellation committed late");
    composition(surface, QStringLiteral("replace"));
    composition(surface, {}, QStringLiteral("must-not-delete"), -1);
    require(text_frames(peer).isEmpty(), "Unsupported replacement mutated PTY input");
    composition(surface, QStringLiteral("before-paste"));
    const auto clipboard = QGuiApplication::clipboard()->text();
    QGuiApplication::clipboard()->setText(QStringLiteral("paste界\nsecond"));
    QKeyEvent paste(QEvent::KeyPress, Qt::Key_V, Qt::MetaModifier);
    QCoreApplication::sendEvent(&surface, &paste);
    QGuiApplication::clipboard()->setText(clipboard);
    const auto pasted = text_frames(peer, QStringLiteral("paste界\nsecond").toUtf8().size());
    if (pasted != QStringLiteral("paste界\nsecond").toUtf8()) {
        std::cerr << "paste=" << pasted.toHex().toStdString()
                  << " focus=" << surface.hasActiveFocus() << " active=" << window.isActive()
                  << " ready=" << f.document.inputReady() << '\n';
        throw std::runtime_error("Paste was split or changed");
    }
    composition(surface, {}, QStringLiteral("late-after-paste"));
    require(text_frames(peer).isEmpty(), "Pre-paste composition committed late");
    composition(surface, QStringLiteral("before-history"));
    f.document.olderHistory();
    composition(surface, {}, QStringLiteral("history-leak"));
    require(!surface.inputMethodQuery(Qt::ImEnabled).toBool() && text_frames(peer).isEmpty(),
            "History browsing retained IME ownership");
    f.document.returnToLive();
    composition(surface, {}, QStringLiteral("late-after-history"));
    require(text_frames(peer).isEmpty(), "History transition retained stale composition");
    composition(surface, QStringLiteral("before-switch"));
    SessionPreview other(QStringLiteral("other"), QStringLiteral("/tmp"), {}, QColor(Qt::white),
                         "");
    surface.setDocument(&other);
    composition(surface, {}, QStringLiteral("wrong-owner"));
    surface.setDocument(&f.document);
    composition(surface, {}, QStringLiteral("stale-owner"));
    require(text_frames(peer).isEmpty(), "Document switch leaked composition");
    composition(surface, QStringLiteral("before-focus"));
    QQuickItem alternate(window.contentItem());
    alternate.forceActiveFocus();
    surface.forceActiveFocus();
    composition(surface, {}, QStringLiteral("late-focus"));
    require(text_frames(peer).isEmpty(), "Focus transition leaked composition");
    // Model the user returning to the terminal before beginning a new
    // composition. Native focus restoration may complete on a later event.
    lapis::desktop::test::activate_test_window(window);
    settle(); // Drain native activation events before beginning an IME transaction.
    surface.forceActiveFocus();
    until([&] { return surface.inputMethodQuery(Qt::ImEnabled).toBool(); });
    composition(surface, QStringLiteral("new"));
    composition(surface, {}, QStringLiteral("新"));
    const auto recovered = text_frames(peer, 3);
    if (recovered != QStringLiteral("新").toUtf8()) {
        std::cerr << "recovered=" << recovered.toHex().toStdString()
                  << " focus=" << surface.hasActiveFocus() << " active=" << window.isActive()
                  << " ready=" << f.document.inputReady() << '\n';
        throw std::runtime_error("Fresh composition did not recover");
    }
    composition(surface, {}, QStringLiteral("✓"));
    require(text_frames(peer, 3) == QStringLiteral("✓").toUtf8(),
            "Recovered terminal rejected commit-only input");
    composition(surface, QStringLiteral("disconnect"));
    peer.socket->abort();
    until([&] { return !f.document.inputReady(); });
    composition(surface, {}, QStringLiteral("disconnected"));
    require(!surface.inputMethodQuery(Qt::ImEnabled).toBool(),
            "Disconnected terminal retained IME ownership");
}
} // namespace
int main(int argc, char** argv) {
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Vulkan);
    QCoreApplication::setAttribute(Qt::AA_MacDontSwapCtrlAndMeta);
    QGuiApplication app(argc, argv);
    try {
        input_contract();
        std::cout << "Qt IME commit/cancel, replacement rejection, paste, history, focus, document "
                     "and disconnect ownership passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
