#include "lapis/supervisor/runtime.hpp"
#include "lapis/supervisor/session_service_launcher.hpp"

#include "launch_spec.hpp"
#include "transport/local_protocol.hpp"

#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLocalServer>
#include <QLocalSocket>
#include <QTemporaryDir>

#include <chrono>
#include <csignal>
#include <cstring>
#include <iostream>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>

namespace {
using namespace lapis::supervisor;
using lapis::session::LaunchSpec;
namespace wire = lapis::session::wire;

struct BindUnavailable {};

void require(bool value, const char* message,
             std::source_location where = std::source_location::current()) {
    if (!value)
        throw std::runtime_error(std::string{"supervisor-session-service failed at line "} +
                                 std::to_string(where.line()) + ": " + message);
}

std::string repeat(char value, std::size_t length) { return std::string(length, value); }

class DeterministicIdentity final : public IdentityProvider {
  public:
    [[nodiscard]] std::string instance_epoch() override { return repeat('3', epoch_hex_bytes); }
    [[nodiscard]] std::string session_id() override { return repeat('4', identity_hex_bytes); }
    [[nodiscard]] std::string session_epoch() override {
        const auto value = static_cast<char>('5' + epoch_serial_++ % 5);
        return repeat(value, identity_hex_bytes);
    }
    [[nodiscard]] std::string spawn_token() override {
        const auto value = static_cast<char>('0' + token_serial_++ % 10);
        return repeat(value, spawn_token_hex_bytes);
    }

  private:
    unsigned token_serial_{0};
    unsigned epoch_serial_{0};
};

DesiredSession record(const std::string& endpoint, const std::string& fingerprint) {
    DesiredSession result;
    result.endpoint = endpoint;
    result.fingerprint = fingerprint;
    result.identity = {repeat('1', identity_hex_bytes), repeat('2', identity_hex_bytes)};
    return result;
}

LaunchSpec launch_spec(const QTemporaryDir& directory) {
    return {.program = QStringLiteral("/bin/cat"),
            .arguments = {},
            .directory = directory.path(),
            .size = {80, 24},
            .agent = lapis::session::AgentMode::terminal};
}

SessionServiceLaunch service_launch(const QTemporaryDir& directory) {
    SessionServiceLaunch result;
    result.service_program = QString::fromLatin1(LAPIS_SUPERVISOR_SESSION_SERVICE);
    result.agent_program = QStringLiteral("/bin/cat");
    result.working_directory = directory.path();
    result.columns = 80;
    result.rows = 24;
    result.codex = false;
    result.claude = false;
    return result;
}

void write_all(QLocalSocket& socket, const QByteArray& bytes) {
    require(socket.write(bytes) == bytes.size() && socket.flush() &&
                (socket.bytesToWrite() == 0 || socket.waitForBytesWritten(2000)),
            "send a complete session frame");
}

bool read_frame(QLocalSocket& socket, QByteArray& buffer, wire::Frame& frame,
                std::chrono::steady_clock::time_point deadline) {
    while (wire::take_frame(buffer, frame))
        return true;
    while (std::chrono::steady_clock::now() < deadline) {
        if (socket.state() != QLocalSocket::ConnectedState)
            return false;
        if (socket.waitForReadyRead(50))
            buffer += socket.readAll();
        if (wire::take_frame(buffer, frame))
            return true;
    }
    return false;
}

wire::Hello attach(QLocalSocket& socket, const DesiredSession& session, bool join) {
    const wire::AttachRequest request{
        .mode = join ? wire::AttachMode::join : wire::AttachMode::reconnect,
        .fingerprint = QByteArray::fromHex(QByteArray::fromStdString(session.fingerprint)),
        .expected = join ? wire::SessionIdentity{}
                         : wire::SessionIdentity{QByteArray::fromHex(QByteArray::fromStdString(
                                                     session.identity.session_id)),
                                                 QByteArray::fromHex(QByteArray::fromStdString(
                                                     session.identity.epoch))},
        .hyperlinks = true,
        .attention_phase = false,
        .paste_transactions = false,
    };
    write_all(socket, wire::frame(wire::Kind::attach, wire::encode_attach(request)));
    QByteArray buffer;
    wire::Frame frame;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    require(read_frame(socket, buffer, frame, deadline), "session attachment receives a frame");
    if (frame.kind == wire::Kind::status)
        throw std::runtime_error("session attachment rejected: " +
                                 wire::decode_status(frame.payload).message.toStdString());
    require(frame.kind == wire::Kind::hello, "session attachment receives hello");
    return wire::decode_hello(frame.payload);
}

wire::SnapshotMessage synchronize(QLocalSocket& socket, QByteArray buffer = {}) {
    wire::Frame frame;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (read_frame(socket, buffer, frame, deadline)) {
        require(frame.kind == wire::Kind::snapshot, "synchronization sends a snapshot");
        const auto snapshot = wire::decode_snapshot_message(frame.payload);
        write_all(socket, wire::frame(wire::Kind::ready,
                                      wire::encode_ready({.attachment = snapshot.attachment,
                                                          .sequence = snapshot.sequence})));
        return snapshot;
    }
    throw std::runtime_error("session synchronization timed out");
}

std::u32string screen_text(const wire::SnapshotMessage& snapshot) {
    std::u32string result;
    for (const auto& cell : snapshot.snapshot.cells) {
        result += snapshot.snapshot.graphemes.substr(cell.text_offset, cell.text_length);
    }
    return result;
}

wire::SnapshotMessage wait_text(QLocalSocket& socket, QByteArray buffer, char32_t marker) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        wire::Frame frame;
        if (read_frame(socket, buffer, frame, deadline) && frame.kind == wire::Kind::snapshot) {
            const auto snapshot = wire::decode_snapshot_message(frame.payload);
            if (screen_text(snapshot).find(marker) != std::u32string::npos)
                return snapshot;
        }
    }
    throw std::runtime_error("detached session output was not replayed");
}

void send_text(QLocalSocket& socket, const wire::Attachment& attachment, char value) {
    const QByteArray text{&value, 1};
    write_all(socket, wire::frame(wire::Kind::text, wire::encode_control({.attachment = attachment,
                                                                          .payload = text})));
}

void detach(QLocalSocket& socket) {
    socket.disconnectFromServer();
    if (socket.state() != QLocalSocket::UnconnectedState)
        require(socket.waitForDisconnected(2000), "session detach is explicit");
}

bool ownership_bytes(const std::string& path, QByteArray& bytes) {
    QFile file{QString::fromStdString(path)};
    if (!file.open(QIODevice::ReadOnly))
        return false;
    bytes = file.readAll();
    return true;
}

void restore_ownership(const std::string& path, const QByteArray& bytes) {
    QFile::remove(QString::fromStdString(path));
    QFile file{QString::fromStdString(path)};
    require(file.open(QIODevice::WriteOnly | QIODevice::NewOnly) &&
                file.write(bytes) == bytes.size() &&
                file.setPermissions(QFile::ReadOwner | QFile::WriteOwner),
            "restore the private supervisor ownership fixture");
}

QTemporaryDir owned_test_directory() {
#if defined(__APPLE__)
    QTemporaryDir directory{QStringLiteral("/private/tmp/lapis-r1-XXXXXX")};
#else
    QTemporaryDir directory{QStringLiteral("/tmp/lapis-r1-XXXXXX")};
#endif
    return directory;
}

void require_bindable_endpoint(const QString& probe) {
    QLocalServer server;
    if (!server.listen(probe)) {
        QFile::remove(probe);
        std::cout << "supervisor-session-service: QLocalServer bind unavailable ("
                  << server.errorString().toStdString() << ")\n";
        throw BindUnavailable{};
    }
    server.close();
    QFile::remove(probe);
}

void adoption_rejects_false_peers(const DesiredSession& session, const SessionServiceLaunch& launch,
                                  const std::string& fingerprint) {
    SessionServiceLauncher real{launch, fingerprint};
    const auto owner = session.endpoint + ".supervisor-owner";
    QByteArray saved;
    require(ownership_bytes(owner, saved), "save the valid ownership record");

    auto identity_changed = session;
    identity_changed.identity.epoch = repeat('7', identity_hex_bytes);
    require(!real.adopt(identity_changed, session.spawn_token), "reject a stale identity");
    QByteArray preserved;
    require(ownership_bytes(owner, preserved) && preserved == saved,
            "a live peer keeps its ownership record across a rejected handshake");
    restore_ownership(owner, saved);

    auto fingerprint_changed = session;
    fingerprint_changed.fingerprint = repeat('b', fingerprint_hex_bytes);
    require(!real.adopt(fingerprint_changed, session.spawn_token), "reject a launch mismatch");
    require(ownership_bytes(owner, preserved) && preserved == saved,
            "a launch mismatch does not orphan the running service");
    restore_ownership(owner, saved);

    QFile dead(QString::fromStdString(owner));
    require(dead.open(QIODevice::WriteOnly | QIODevice::Truncate), "replace ownership for death");
    const std::string bytes = "LAPIS-SUP-OWNER-1\n" + session.spawn_token + "\n99999999";
    require(dead.write(QByteArray::fromStdString(bytes)) == static_cast<qint64>(bytes.size()),
            "write a dead PID record");
    dead.close();
    require(!real.adopt(session, session.spawn_token), "reject a dead ownership PID");
    require(!ownership_bytes(owner, preserved), "a dead ownership PID removes its record");
    restore_ownership(owner, saved);
}

void wait_gone(pid_t pid) {
    for (int attempt = 0; attempt < 300; ++attempt) {
        int status = 0;
        if (::waitpid(pid, &status, WNOHANG) == pid)
            return;
        if (::kill(pid, 0) != 0)
            return;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    throw std::runtime_error("supervised processes did not stop");
}

void supervised_service_clients_adopt_and_stop() {
    QTemporaryDir directory = owned_test_directory();
    require(directory.isValid(), "create an owned private test directory");
    const QString bind_probe = directory.filePath(QStringLiteral("bind.sock"));
    require_bindable_endpoint(bind_probe);
    const auto endpoint = directory.filePath(QStringLiteral("s.sock")).toStdString();
    const auto expected =
        lapis::session::launch_fingerprint(lapis::session::validate_launch(launch_spec(directory)));
    const auto fingerprint = QString::fromLatin1(expected.toHex()).toStdString();
    auto desired = record(endpoint, fingerprint);
    const auto launch = service_launch(directory);
    auto owner_registry = std::make_shared<SupervisorRegistry>(
        std::make_shared<MemoryStateStorage>(), std::make_shared<JsonStateCodec>(),
        std::make_shared<DeterministicIdentity>());
    const auto launcher = std::make_shared<SessionServiceLauncher>(launch, fingerprint);
    pid_t service_pid = -1;
    quint64 child_pid = 0;
    wire::Attachment authoritative;
    ChildProcess directly_launched;
    try {
        auto bootstrapped = owner_registry->start(desired);
        desired.spawn_token = bootstrapped.session->spawn_token;
        directly_launched =
            launcher->launch(*bootstrapped.session, bootstrapped.session->spawn_token);
    } catch (const std::exception& error) {
        std::cerr << "supervisor service launch: " << error.what() << '\n';
        throw;
    }
    require(directly_launched.pid > 0, "direct supervisor launch proves the exact failure");
    {
        SupervisorRuntime runtime{owner_registry, launcher};
        // Construction converges over the already-launched child; calling
        // bootstrap here would start a second registry admission and rotate the
        // spawn token away from the service's ownership record.
        service_pid = runtime.child_pid();
        require(service_pid > 0, "supervisor starts the real service");
        require(runtime.converge() == Convergence::converged, "the live service converges");

        QLocalSocket primary;
        primary.connectToServer(QString::fromStdString(endpoint), QLocalSocket::ReadWrite);
        require(primary.waitForConnected(5000), "authoritative client reaches the service");
        const auto hello = attach(primary, desired, false);
        require(hello.attachment.identity ==
                    wire::SessionIdentity{
                        QByteArray::fromHex(QByteArray::fromStdString(desired.identity.session_id)),
                        QByteArray::fromHex(QByteArray::fromStdString(desired.identity.epoch))},
                "hello reports the supervisor-selected identity");
        require(hello.pid > 0 && hello.pid != static_cast<quint64>(service_pid),
                "hello reports the real terminal child");
        authoritative = hello.attachment;
        const auto screen = synchronize(primary);
        require(screen.attachment == authoritative,
                "snapshot matches the authoritative attachment");
        send_text(primary, authoritative, 'R');
        const auto echoed = wait_text(primary, {}, U'R');
        require(screen_text(echoed).find(U'R') != std::u32string::npos,
                "authoritative input reaches the terminal");
        child_pid = hello.pid;
        detach(primary);

        adoption_rejects_false_peers(desired, launch, fingerprint);

        QLocalSocket joined;
        joined.connectToServer(QString::fromStdString(endpoint), QLocalSocket::ReadWrite);
        require(joined.waitForConnected(5000), "join client reaches the service");
        const auto join_hello = attach(joined, desired, true);
        require(join_hello.pid == child_pid, "join sees the same terminal child");
        const auto join_screen = synchronize(joined);
        send_text(joined, join_screen.attachment, U'J');
        const auto joined_echo = wait_text(joined, {}, U'J');
        require(screen_text(joined_echo).find(U'J') != std::u32string::npos,
                "joined input reaches the terminal");
        detach(joined);

        QLocalSocket rejoined;
        rejoined.connectToServer(QString::fromStdString(endpoint), QLocalSocket::ReadWrite);
        require(rejoined.waitForConnected(5000), "rejoined join client reaches the service");
        const auto rejoin_hello = attach(rejoined, desired, true);
        require(rejoin_hello.attachment.identity == join_screen.attachment.identity &&
                    rejoin_hello.attachment.generation > join_screen.attachment.generation,
                "rejoin advances only the join generation");
        require(rejoin_hello.pid == child_pid, "rejoin preserves the terminal child");
        static_cast<void>(synchronize(rejoined));
        detach(rejoined);

        QLocalSocket reconnected;
        reconnected.connectToServer(QString::fromStdString(endpoint), QLocalSocket::ReadWrite);
        require(reconnected.waitForConnected(5000), "reconnecting client reaches the service");
        const auto reconnect_hello = attach(reconnected, desired, false);
        require(reconnect_hello.pid == child_pid &&
                    reconnect_hello.attachment.identity == authoritative.identity,
                "authoritative reconnect preserves the child identity");
        require(reconnect_hello.attachment.generation > authoritative.generation,
                "authoritative reconnect advances its generation");
        const auto reconnect_screen = synchronize(reconnected);
        send_text(reconnected, reconnect_screen.attachment, U'A');
        const auto reconnected_echo = wait_text(reconnected, {}, U'A');
        require(screen_text(reconnected_echo).find(U'A') != std::u32string::npos &&
                    screen_text(reconnected_echo).find(U'J') != std::u32string::npos,
                "detached output remains visible after reconnect");
    }

    require(::kill(service_pid, 0) == 0, "destroying the runtime leaves the service alive");
    SupervisorRuntime reconstructed{owner_registry, launcher};
    require(reconstructed.converge() == Convergence::converged &&
                reconstructed.child_pid() == service_pid,
            "a reconstructed supervisor adopts the same real service");

    require(reconstructed.state().session.has_value(), "adopted state retains the session");
    const QJsonObject stop_json{
        {QStringLiteral("version"), 1},
        {QStringLiteral("action"), QStringLiteral("stop")},
        {QStringLiteral("supervisor_epoch"),
         QString::fromStdString(reconstructed.state().instance_epoch)},
        {QStringLiteral("token"),
         QString::fromStdString(reconstructed.state().session->spawn_token)},
        {QStringLiteral("session"), QJsonValue::Null},
    };
    const auto stop_bytes = QJsonDocument{stop_json}.toJson(QJsonDocument::Compact);
    const std::string stop_request{stop_bytes.constData(),
                                   static_cast<std::size_t>(stop_bytes.size())};
    const auto stopped_reply_bytes = reconstructed.control(::geteuid(), stop_request);
    const QJsonDocument stopped_reply =
        QJsonDocument::fromJson(QByteArray::fromStdString(stopped_reply_bytes));
    require(stopped_reply.object().value(QStringLiteral("status")).toString() ==
                    QStringLiteral("applied") &&
                reconstructed.state().session->desired_state == DesiredState::stopped,
            "authenticated control records the explicit stop");
    require(reconstructed.converge() == Convergence::stopped && reconstructed.child_pid() == -1,
            "explicit supervisor stop terminates the adopted service");
    wait_gone(service_pid);
    wait_gone(static_cast<pid_t>(child_pid));
    struct stat owner_info{};
    require(::lstat((endpoint + ".supervisor-owner").c_str(), &owner_info) != 0 && errno == ENOENT,
            "explicit stop removes service ownership");
}

// A crashed service becomes a zombie child of this test process, so a bare
// kill(pid, 0) probe keeps reporting it alive and convergence would never take
// the restart path. The launcher must reap its own children, and the admitted
// replacement must rotate the epoch and spawn token.
void crashed_service_restarts_with_rotated_identity() {
    QTemporaryDir directory = owned_test_directory();
    require(directory.isValid(), "create an owned private test directory");
    require_bindable_endpoint(directory.filePath(QStringLiteral("bind.sock")));
    const auto endpoint = directory.filePath(QStringLiteral("restart.sock")).toStdString();
    const auto expected =
        lapis::session::launch_fingerprint(lapis::session::validate_launch(launch_spec(directory)));
    const auto fingerprint = QString::fromLatin1(expected.toHex()).toStdString();
    auto desired = record(endpoint, fingerprint);
    const auto launch = service_launch(directory);
    auto owner_registry = std::make_shared<SupervisorRegistry>(
        std::make_shared<MemoryStateStorage>(), std::make_shared<JsonStateCodec>(),
        std::make_shared<DeterministicIdentity>());
    const auto launcher = std::make_shared<SessionServiceLauncher>(launch, fingerprint);
    SupervisorRuntime runtime{owner_registry, launcher};
    const auto started = runtime.bootstrap(desired);
    const auto first_pid = runtime.child_pid();
    require(first_pid > 0, "the first real service starts");
    require(started.session.has_value(), "the started session is persisted");
    const auto original_epoch = started.session->identity.epoch;
    const auto original_token = started.session->spawn_token;

    require(::kill(first_pid, SIGKILL) == 0, "crash the first real service");
    bool crashed = false;
    for (int attempt = 0; attempt < 300 && !crashed; ++attempt) {
        // SIGKILL delivery is asynchronous; converge once the dead service is
        // visible as an unreaped zombie so alive() cannot hide behind it.
        crashed = ::kill(first_pid, 0) == 0;
        if (!crashed)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    require(crashed, "the crashed service is visible before convergence");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    require(runtime.converge() == Convergence::restarted, "a crashed real service restarts");
    const auto second_pid = runtime.child_pid();
    require(second_pid > 0 && second_pid != first_pid, "the replacement is a new service process");
    const auto& rotated = owner_registry->state().session;
    require(rotated.has_value() && rotated->identity.epoch != original_epoch,
            "the crash restart rotates the service epoch");
    require(rotated->spawn_token != original_token, "the crash restart rotates the spawn token");

    const QJsonObject stop_json{
        {QStringLiteral("version"), 1},
        {QStringLiteral("action"), QStringLiteral("stop")},
        {QStringLiteral("supervisor_epoch"),
         QString::fromStdString(owner_registry->state().instance_epoch)},
        {QStringLiteral("token"), QString::fromStdString(rotated->spawn_token)},
        {QStringLiteral("session"), QJsonValue::Null},
    };
    const auto stop_bytes = QJsonDocument{stop_json}.toJson(QJsonDocument::Compact);
    const std::string stop_request{stop_bytes.constData(),
                                   static_cast<std::size_t>(stop_bytes.size())};
    const auto stopped_reply_bytes = runtime.control(::geteuid(), stop_request);
    const QJsonDocument stopped_reply =
        QJsonDocument::fromJson(QByteArray::fromStdString(stopped_reply_bytes));
    require(stopped_reply.object().value(QStringLiteral("status")).toString() ==
                    QStringLiteral("applied") &&
                runtime.state().session->desired_state == DesiredState::stopped,
            "authenticated control stops the replacement service");
    require(runtime.converge() == Convergence::stopped && runtime.child_pid() == -1,
            "explicit stop terminates the replacement service");
    wait_gone(second_pid);
    struct stat owner_info{};
    require(::lstat((endpoint + ".supervisor-owner").c_str(), &owner_info) != 0 && errno == ENOENT,
            "explicit stop removes the replacement ownership record");
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication application{argc, argv};
    try {
        supervised_service_clients_adopt_and_stop();
        crashed_service_restarts_with_rotated_identity();
        std::cout << "supervisor-session-service: ok\n";
        return 0;
    } catch (const BindUnavailable&) {
        return 75;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
