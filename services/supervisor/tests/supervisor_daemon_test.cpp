#include "lapis/supervisor/birth.hpp"
#include "launch_spec.hpp"
#include "transport/local_protocol.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLocalServer>
#include <QLocalSocket>
#include <QTemporaryDir>

#include <array>
#include <chrono>
#include <csignal>
#include <cstring>
#include <iostream>
#include <source_location>
#include <spawn.h>
#include <stdexcept>
#include <string>
#include <sys/poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

extern char** environ;

namespace {
using namespace lapis::supervisor;
using lapis::session::LaunchSpec;
namespace wire = lapis::session::wire;

void require(bool value, const char* message,
             std::source_location where = std::source_location::current()) {
    if (!value)
        throw std::runtime_error(std::string{"supervisor-daemon failed at line "} +
                                 std::to_string(where.line()) + ": " + message);
}

struct BindUnavailable : std::runtime_error {
    BindUnavailable() : std::runtime_error{"QLocalServer bind unavailable"} {}
};

struct ControlClient {
    int descriptor{-1};

    explicit ControlClient(const std::string& endpoint) {
        descriptor = ::socket(AF_UNIX, SOCK_STREAM, 0);
        require(descriptor >= 0, "create supervisor control client");
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        require(endpoint.size() < sizeof(address.sun_path), "control endpoint path length");
        std::memcpy(address.sun_path, endpoint.c_str(), endpoint.size() + 1);
        for (int attempt = 0; attempt < 100; ++attempt) {
            if (::connect(descriptor, reinterpret_cast<const sockaddr*>(&address),
                          sizeof(address)) == 0)
                return;
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
        throw std::runtime_error("connect supervisor control endpoint");
    }

    ~ControlClient() {
        if (descriptor >= 0)
            static_cast<void>(::close(descriptor));
    }
    ControlClient(const ControlClient&) = delete;
    ControlClient& operator=(const ControlClient&) = delete;
};

void write_all(int descriptor, const char* bytes, std::size_t size) {
    std::size_t done = 0;
    while (done < size) {
        const auto count = ::write(descriptor, bytes + done, size - done);
        if (count > 0) {
            done += static_cast<std::size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        throw std::runtime_error("send a complete supervisor control frame");
    }
}

void send_frame(ControlClient& client, const std::string& payload) {
    std::array<unsigned char, 4> header{};
    const auto length = static_cast<std::uint32_t>(payload.size());
    header[0] = static_cast<unsigned char>((length >> 24U) & 0xffU);
    header[1] = static_cast<unsigned char>((length >> 16U) & 0xffU);
    header[2] = static_cast<unsigned char>((length >> 8U) & 0xffU);
    header[3] = static_cast<unsigned char>(length & 0xffU);
    write_all(client.descriptor, reinterpret_cast<const char*>(header.data()), header.size());
    write_all(client.descriptor, payload.data(), payload.size());
}

std::string receive_reply(ControlClient& client) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{25};
    std::string reply;
    while (std::chrono::steady_clock::now() < deadline) {
        pollfd wait{client.descriptor, POLLIN, 0};
        const int ready = ::poll(&wait, 1, 1000);
        if (ready < 0 && errno == EINTR)
            continue;
        require(ready >= 0, "wait for supervisor control reply");
        if (ready == 0)
            continue;
        std::array<char, 4096> bytes{};
        const auto count = ::read(client.descriptor, bytes.data(), bytes.size());
        if (count > 0) {
            reply.append(bytes.data(), static_cast<std::size_t>(count));
            continue;
        }
        if (count == 0 && !reply.empty())
            return reply;
        throw std::runtime_error("receive supervisor control reply");
    }
    throw std::runtime_error("supervisor control reply timed out");
}

void expect_closed_after_oversized_frame(ControlClient& client) {
    const std::array<char, 4> header{{static_cast<char>(0xff), static_cast<char>(0xff),
                                      static_cast<char>(0xff), static_cast<char>(0xff)}};
    write_all(client.descriptor, header.data(), header.size());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{3};
    while (std::chrono::steady_clock::now() < deadline) {
        pollfd wait{client.descriptor, POLLIN, 0};
        const int ready = ::poll(&wait, 1, 100);
        if (ready == 0)
            continue;
        char byte = 0;
        const auto count = ::read(client.descriptor, &byte, 1);
        if (count == 0)
            return;
        throw std::runtime_error("oversized supervisor birth frame has no reply");
    }
    throw std::runtime_error("oversized supervisor birth frame was not closed");
}

QJsonObject birth_object(const QString& fingerprint, const QJsonObject& service,
                         const QString& endpoint, const QString& program) {
    return QJsonObject{{QStringLiteral("version"), 1},
                       {QStringLiteral("action"), QStringLiteral("birth")},
                       {QStringLiteral("endpoint"), endpoint},
                       {QStringLiteral("fingerprint"), fingerprint},
                       {QStringLiteral("agent"),
                        QJsonObject{{QStringLiteral("mode"), QStringLiteral("terminal")},
                                    {QStringLiteral("program"), program},
                                    {QStringLiteral("arguments"), QJsonArray{}},
                                    {QStringLiteral("working_directory"), QDir::currentPath()},
                                    {QStringLiteral("columns"), 80},
                                    {QStringLiteral("rows"), 24}}},
                       {QStringLiteral("service"), service}};
}

std::string birth_json(const QString& fingerprint, const QJsonObject& service,
                       const QString& program = QStringLiteral("/bin/cat")) {
    const auto bytes =
        QJsonDocument{birth_object(fingerprint, service,
                                   QDir::current().filePath(QStringLiteral("session.sock")),
                                   program)}
            .toJson(QJsonDocument::Compact);
    return {bytes.constData(), static_cast<std::size_t>(bytes.size())};
}

std::string birth_json_at(const QString& fingerprint, const QJsonObject& service,
                          const QString& endpoint) {
    const auto bytes =
        QJsonDocument{birth_object(fingerprint, service, endpoint, QStringLiteral("/bin/cat"))}
            .toJson(QJsonDocument::Compact);
    return {bytes.constData(), static_cast<std::size_t>(bytes.size())};
}

QJsonObject reply_object(const std::string& bytes) {
    const auto document = QJsonDocument::fromJson(QByteArray::fromStdString(bytes));
    require(document.isObject(), "supervisor birth reply is JSON");
    return document.object();
}

pid_t start_daemon(const std::string& state,
                   const std::string& control) { // NOLINT(bugprone-easily-swappable-parameters)
    std::vector<std::string> storage{LAPIS_SUPERVISOR_DAEMON, "--state-directory=" + state,
                                     "--control-endpoint=" + control};
    std::vector<char*> arguments;
    arguments.reserve(storage.size() + 1);
    for (const auto& argument : storage)
        arguments.push_back(const_cast<char*>(argument.c_str()));
    arguments.push_back(nullptr);
    pid_t pid = -1;
    require(::posix_spawn(&pid, storage.front().c_str(), nullptr, nullptr, arguments.data(),
                          environ) == 0,
            "start the independent supervisor daemon");
    const auto started_at = std::chrono::steady_clock::now();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{30};
    while (std::chrono::steady_clock::now() < deadline) {
        int status = 0;
        const pid_t changed = ::waitpid(pid, &status, WNOHANG);
        require(changed != pid, "the supervisor daemon remains running during startup");
        struct stat info{};
        if (::lstat(control.c_str(), &info) == 0 && S_ISSOCK(info.st_mode)) {
            const int probe = ::socket(AF_UNIX, SOCK_STREAM, 0);
            if (probe >= 0) {
                sockaddr_un address{};
                address.sun_family = AF_UNIX;
                std::memcpy(address.sun_path, control.c_str(), control.size() + 1);
                const bool ready =
                    ::connect(probe, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
                static_cast<void>(::close(probe));
                if (ready)
                    return pid;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started_at);
    throw std::runtime_error("supervisor daemon did not bind its control endpoint after " +
                             std::to_string(elapsed.count()) + "ms");
}

pid_t service_pid_from_ownership(const std::string& endpoint) {
    QFile file{QString::fromStdString(endpoint + ".supervisor-owner")};
    require(file.open(QIODevice::ReadOnly), "read supervisor service ownership");
    const QStringList lines = QString::fromUtf8(file.readAll()).split(QLatin1Char{'\n'});
    require(lines.size() == 3 && lines[0] == QLatin1String("LAPIS-SUP-OWNER-1") &&
                lines[1].size() == static_cast<qsizetype>(spawn_token_hex_bytes) &&
                !lines[2].isEmpty(),
            "parse supervisor service ownership");
    bool parsed = false;
    const int value = lines[2].toInt(&parsed);
    require(parsed && value > 0, "parse the supervisor service PID");
    return value;
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

wire::Hello attach(QLocalSocket& socket, const std::string& fingerprint,
                   const std::string& session_id, const std::string& epoch,
                   bool join) { // NOLINT(bugprone-easily-swappable-parameters)
    const wire::AttachRequest request{
        .mode = join ? wire::AttachMode::join : wire::AttachMode::reconnect,
        .fingerprint = QByteArray::fromHex(QByteArray::fromStdString(fingerprint)),
        .expected =
            join ? wire::SessionIdentity{}
                 : wire::SessionIdentity{QByteArray::fromHex(QByteArray::fromStdString(session_id)),
                                         QByteArray::fromHex(QByteArray::fromStdString(epoch))},
        .hyperlinks = true,
        .attention_phase = false,
        .paste_transactions = false,
    };
    write_all(socket, wire::frame(wire::Kind::attach, wire::encode_attach(request)));
    QByteArray buffer;
    wire::Frame frame;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    require(read_frame(socket, buffer, frame, deadline), "session attachment receives a frame");
    if (frame.kind == wire::Kind::status)
        throw std::runtime_error("session attachment rejected: " +
                                 wire::decode_status(frame.payload).message.toStdString());
    require(frame.kind == wire::Kind::hello, "session attachment receives hello");
    return wire::decode_hello(frame.payload);
}

wire::SnapshotMessage synchronize(QLocalSocket& socket) {
    QByteArray buffer;
    wire::Frame frame;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
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

wire::SnapshotMessage wait_text(QLocalSocket& socket, char32_t marker) {
    QByteArray buffer;
    wire::Frame frame;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (read_frame(socket, buffer, frame, deadline)) {
        if (frame.kind != wire::Kind::snapshot)
            continue;
        const auto snapshot = wire::decode_snapshot_message(frame.payload);
        if (screen_text(snapshot).find(marker) != std::u32string::npos)
            return snapshot;
    }
    throw std::runtime_error("terminal echo was not observed");
}

void send_text(QLocalSocket& socket, const wire::Attachment& attachment, char32_t value) {
    const QString text{QChar{value}};
    const auto bytes = text.toUtf8();
    write_all(socket, wire::frame(wire::Kind::text, wire::encode_control({.attachment = attachment,
                                                                          .payload = bytes})));
}

void detach(QLocalSocket& socket) {
    socket.disconnectFromServer();
    if (socket.state() != QLocalSocket::UnconnectedState)
        require(socket.waitForDisconnected(2000), "session detach is explicit");
}

void require_bindable(const QString& endpoint) {
    QFile::remove(endpoint);
    QLocalServer server;
    if (!server.listen(endpoint)) {
        std::cout << "supervisor-daemon: QLocalServer bind unavailable ("
                  << server.errorString().toStdString() << ")\n";
        throw BindUnavailable{};
    }
    server.close();
    QFile::remove(endpoint);
}

void wait_gone(pid_t pid) {
    for (int attempt = 0; attempt < 500; ++attempt) {
        if (::kill(pid, 0) != 0 && errno == ESRCH)
            return;
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    throw std::runtime_error("a supervised process did not stop");
}

QTemporaryDir owned_test_directory(const QString& pattern) {
#if defined(__APPLE__)
    QTemporaryDir directory{QStringLiteral("/private/tmp/") + pattern};
    if (directory.isValid())
        return directory;
#endif
    return QTemporaryDir{QStringLiteral("/tmp/") + pattern};
}

void daemon_birth_refuses_unsafe_requests(const std::string& control, const QString& fingerprint) {
    const QJsonObject empty_service{{QStringLiteral("environment"), QJsonObject{}}};
    {
        ControlClient client{control};
        send_frame(client, "{");
        require(reply_object(receive_reply(client)).value(QStringLiteral("status")).toString() ==
                    QLatin1String("rejected"),
                "malformed birth JSON is rejected");
    }
    {
        ControlClient client{control};
        expect_closed_after_oversized_frame(client);
    }
    {
        ControlClient client{control};
        send_frame(client, birth_json(fingerprint, empty_service, QStringLiteral("   ")));
        require(reply_object(receive_reply(client)).value(QStringLiteral("status")).toString() ==
                    QLatin1String("rejected"),
                "a whitespace-only launch value is rejected");
    }
    {
        ControlClient client{control};
        send_frame(client, birth_json(fingerprint, empty_service, QStringLiteral(" /bin/cat")));
        require(reply_object(receive_reply(client)).value(QStringLiteral("status")).toString() ==
                    QLatin1String("rejected"),
                "surrounding whitespace in a launch value is rejected");
    }
    {
        const QJsonObject environment{
            {QStringLiteral("environment"),
             QJsonObject{{QStringLiteral("LAPIS_TEST"), QStringLiteral("1")}}}};
        ControlClient client{control};
        send_frame(client, birth_json(fingerprint, environment));
        require(reply_object(receive_reply(client)).value(QStringLiteral("status")).toString() ==
                    QLatin1String("rejected"),
                "a nonempty service environment is rejected");
    }
    {
        ControlClient client{control};
        send_frame(client, birth_json(QString::fromStdString(std::string(64, 'b')), empty_service));
        require(reply_object(receive_reply(client)).value(QStringLiteral("status")).toString() ==
                    QLatin1String("rejected"),
                "a launch fingerprint mismatch is rejected");
    }
    {
        const QJsonObject service{{QStringLiteral("program"), QStringLiteral("/bin/false")},
                                  {QStringLiteral("environment"), QJsonObject{}}};
        ControlClient client{control};
        send_frame(client, birth_json(fingerprint, service));
        require(reply_object(receive_reply(client)).value(QStringLiteral("status")).toString() ==
                    QLatin1String("rejected"),
                "a caller-selected service program is rejected");
    }
}

void failed_service_birth_never_reports_born() {
    const QString previous = QDir::currentPath();
    QTemporaryDir directory =
        owned_test_directory(QStringLiteral("lapis-r1-daemon-failure-XXXXXX"));
    require(directory.isValid(), "create the failed-birth test directory");
    QDir::setCurrent(directory.path());
    const std::string control = directory.filePath(QStringLiteral("control.sock")).toStdString();
    const QString normal_endpoint = directory.filePath(QStringLiteral("session.sock"));
    const QString impossible_endpoint =
        directory.filePath(QStringLiteral("very-long/") + QString{120, QLatin1Char{'x'}} +
                           QStringLiteral("/session.sock"));
    const LaunchSpec launch{.program = QStringLiteral("/bin/cat"),
                            .arguments = {},
                            .directory = directory.path(),
                            .size = {80, 24},
                            .agent = lapis::session::AgentMode::terminal};
    const QString fingerprint = QString::fromLatin1(
        lapis::session::launch_fingerprint(lapis::session::validate_launch(launch)).toHex());
    const pid_t daemon = start_daemon(directory.path().toStdString(), control);
    const QJsonObject service{{QStringLiteral("environment"), QJsonObject{}}};
    {
        ControlClient client{control};
        send_frame(client, birth_json_at(fingerprint, service, impossible_endpoint));
        require(reply_object(receive_reply(client)).value(QStringLiteral("status")).toString() ==
                    QLatin1String("rejected"),
                "a service launch failure is not reported as born");
    }
    {
        ControlClient client{control};
        send_frame(client, birth_json_at(fingerprint, service, normal_endpoint));
        require(reply_object(receive_reply(client)).value(QStringLiteral("status")).toString() ==
                    QLatin1String("rejected"),
                "a failed birth consumes the one-session slot");
    }
    require(::kill(daemon, SIGTERM) == 0, "stop the failed-birth daemon");
    int status = 0;
    require(::waitpid(daemon, &status, 0) == daemon && WIFEXITED(status) &&
                WEXITSTATUS(status) == 0,
            "the failed-birth daemon exits cleanly");
    QDir::setCurrent(previous);
}

void daemon_birth_and_real_service_clients() {
    QTemporaryDir directory = owned_test_directory(QStringLiteral("lapis-r1-daemon-XXXXXX"));
    require(directory.isValid(), "create the private daemon test directory");
    QDir::setCurrent(directory.path());
    const std::string control = directory.filePath(QStringLiteral("control.sock")).toStdString();
    const std::string endpoint = directory.filePath(QStringLiteral("session.sock")).toStdString();
    require_bindable(directory.filePath(QStringLiteral("bind.sock")));
    const LaunchSpec launch{.program = QStringLiteral("/bin/cat"),
                            .arguments = {},
                            .directory = directory.path(),
                            .size = {80, 24},
                            .agent = lapis::session::AgentMode::terminal};
    const QString fingerprint = QString::fromLatin1(
        lapis::session::launch_fingerprint(lapis::session::validate_launch(launch)).toHex());
    pid_t daemon = -1;
    try {
        daemon = start_daemon(directory.path().toStdString(), control);
        daemon_birth_refuses_unsafe_requests(control, fingerprint);

        std::string session_id;
        std::string epoch;
        {
            const QJsonObject first_service{{QStringLiteral("environment"), QJsonObject{}}};
            ControlClient client{control};
            send_frame(client, birth_json(fingerprint, first_service));
            const auto reply = reply_object(receive_reply(client));
            require(reply.size() == 4 &&
                        reply.value(QStringLiteral("status")).toString() == QLatin1String("born") &&
                        !reply.contains(QStringLiteral("token")),
                    "birth replies with selected identity and no spawn token");
            session_id = reply.value(QStringLiteral("session_id")).toString().toStdString();
            epoch = reply.value(QStringLiteral("epoch")).toString().toStdString();
            require(valid_hex(session_id, identity_hex_bytes) &&
                        valid_hex(epoch, identity_hex_bytes),
                    "birth returns bounded supervisor identity");
        }
        const pid_t service = service_pid_from_ownership(endpoint);
        require(service > 0 && service != daemon, "daemon and service are separate processes");

        {
            const QJsonObject duplicate_service{{QStringLiteral("environment"), QJsonObject{}}};
            ControlClient client{control};
            send_frame(client, birth_json(fingerprint, duplicate_service));
            require(
                reply_object(receive_reply(client)).value(QStringLiteral("status")).toString() ==
                    QLatin1String("rejected"),
                "a second active birth is rejected");
        }

        QLocalSocket primary;
        primary.connectToServer(QString::fromStdString(endpoint), QLocalSocket::ReadWrite);
        require(primary.waitForConnected(5000), "authoritative client reaches the born service");
        const auto first_hello =
            attach(primary, fingerprint.toStdString(), session_id, epoch, false);
        require(first_hello.pid > 0 && first_hello.pid != static_cast<quint64>(daemon) &&
                    first_hello.pid != static_cast<quint64>(service),
                "hello preserves a terminal child distinct from daemon and service");
        const auto first_screen = synchronize(primary);
        send_text(primary, first_screen.attachment, U'R');
        require(screen_text(wait_text(primary, U'R')).find(U'R') != std::u32string::npos,
                "authoritative input reaches the terminal child");

        QLocalSocket joined;
        joined.connectToServer(QString::fromStdString(endpoint), QLocalSocket::ReadWrite);
        require(joined.waitForConnected(5000), "a second client joins the born service");
        const auto joined_hello =
            attach(joined, fingerprint.toStdString(), session_id, epoch, true);
        require(joined_hello.pid == first_hello.pid, "joined client sees the same terminal child");
        const auto joined_screen = synchronize(joined);
        send_text(joined, joined_screen.attachment, U'J');
        require(screen_text(wait_text(joined, U'J')).find(U'J') != std::u32string::npos,
                "joined input reaches the terminal child");

        detach(primary);
        QLocalSocket reconnected;
        reconnected.connectToServer(QString::fromStdString(endpoint), QLocalSocket::ReadWrite);
        require(reconnected.waitForConnected(5000), "authoritative reconnect reaches the service");
        const auto reconnect_hello =
            attach(reconnected, fingerprint.toStdString(), session_id, epoch, false);
        require(reconnect_hello.pid == first_hello.pid &&
                    reconnect_hello.attachment.identity == first_hello.attachment.identity,
                "authoritative reconnect preserves terminal child and session identity");
        require(reconnect_hello.attachment.generation > first_hello.attachment.generation,
                "authoritative reconnect advances attachment generation");
        static_cast<void>(synchronize(reconnected));
        detach(reconnected);
        detach(joined);

        require(::kill(daemon, SIGTERM) == 0, "request explicit daemon stop");
        int status = 0;
        const pid_t stopped = ::waitpid(daemon, &status, 0);
        require(stopped == daemon && WIFEXITED(status) && WEXITSTATUS(status) == 0,
                "daemon exits successfully after explicit stop");
        wait_gone(service);
        wait_gone(static_cast<pid_t>(first_hello.pid));
        struct stat ownership{};
        require(::lstat((endpoint + ".supervisor-owner").c_str(), &ownership) != 0 &&
                    errno == ENOENT,
                "explicit stop removes the private service ownership record");
        struct stat control_file{};
        require(::lstat(control.c_str(), &control_file) != 0 && errno == ENOENT,
                "daemon shutdown removes its control endpoint");
        struct stat state_file{};
        require(::lstat((directory.path().toStdString() + "/supervisor.json").c_str(),
                        &state_file) == 0 &&
                    S_ISREG(state_file.st_mode) && (state_file.st_mode & 07777U) == 0600U,
                "explicit stop leaves private persisted desired state");
    } catch (...) {
        if (daemon > 0) {
            static_cast<void>(::kill(daemon, SIGTERM));
            int status = 0;
            static_cast<void>(::waitpid(daemon, &status, 0));
        }
        throw;
    }
}

void daemon_sigkill_reconstructs_the_live_service() {
    const QString previous = QDir::currentPath();
    QTemporaryDir directory =
        owned_test_directory(QStringLiteral("lapis-r1-daemon-restart-XXXXXX"));
    require(directory.isValid(), "create the daemon restart test directory");
    QDir::setCurrent(directory.path());
    const std::string control = directory.filePath(QStringLiteral("control.sock")).toStdString();
    const std::string endpoint = directory.filePath(QStringLiteral("session.sock")).toStdString();
    require_bindable(directory.filePath(QStringLiteral("bind.sock")));
    const LaunchSpec launch{.program = QStringLiteral("/bin/cat"),
                            .arguments = {},
                            .directory = directory.path(),
                            .size = {80, 24},
                            .agent = lapis::session::AgentMode::terminal};
    const QString fingerprint = QString::fromLatin1(
        lapis::session::launch_fingerprint(lapis::session::validate_launch(launch)).toHex());
    const QJsonObject birth_service{{QStringLiteral("environment"), QJsonObject{}}};
    pid_t first_daemon = -1;
    pid_t replacement_daemon = -1;
    pid_t service_pid = -1;
    quint64 terminal_child = 0;
    std::string session_id;
    std::string epoch;
    QByteArray ownership_before_crash;
    try {
        first_daemon = start_daemon(directory.path().toStdString(), control);
        {
            ControlClient client{control};
            send_frame(client, birth_json(fingerprint, birth_service));
            const auto reply = reply_object(receive_reply(client));
            require(reply.value(QStringLiteral("status")).toString() == QLatin1String("born"),
                    "the first daemon births a service");
            session_id = reply.value(QStringLiteral("session_id")).toString().toStdString();
            epoch = reply.value(QStringLiteral("epoch")).toString().toStdString();
        }
        service_pid = service_pid_from_ownership(endpoint);
        QFile ownership_file{QString::fromStdString(endpoint + ".supervisor-owner")};
        require(ownership_file.open(QIODevice::ReadOnly), "read ownership before the daemon crash");
        ownership_before_crash = ownership_file.readAll();
        ownership_file.close();

        QLocalSocket client;
        client.connectToServer(QString::fromStdString(endpoint), QLocalSocket::ReadWrite);
        require(client.waitForConnected(5000), "the pre-crash client reaches the service");
        const auto hello = attach(client, fingerprint.toStdString(), session_id, epoch, false);
        terminal_child = hello.pid;
        static_cast<void>(synchronize(client));
        send_text(client, hello.attachment, U'B');
        require(screen_text(wait_text(client, U'B')).find(U'B') != std::u32string::npos,
                "the pre-crash service owns a live terminal child");
        detach(client);

        require(::kill(first_daemon, SIGKILL) == 0, "crash the daemon without cleanup");
        int status = 0;
        require(::waitpid(first_daemon, &status, 0) == first_daemon, "reap the crashed daemon");
        first_daemon = -1;
        require(::kill(service_pid, 0) == 0 && ::kill(static_cast<pid_t>(terminal_child), 0) == 0,
                "the daemon crash preserves the service and terminal child");
        QFile next_ownership{QString::fromStdString(endpoint + ".supervisor-owner")};
        require(next_ownership.open(QIODevice::ReadOnly), "read ownership after the daemon crash");
        require(next_ownership.readAll() == ownership_before_crash,
                "the daemon crash preserves service ownership bytes");
        next_ownership.close();

        replacement_daemon = start_daemon(directory.path().toStdString(), control);
        require(service_pid_from_ownership(endpoint) == service_pid,
                "the replacement daemon adopts the same service");
        {
            ControlClient control_client{control};
            send_frame(control_client, birth_json(fingerprint, birth_service));
            require(reply_object(receive_reply(control_client))
                            .value(QStringLiteral("status"))
                            .toString() == QLatin1String("rejected"),
                    "reconstruction does not expose a second birth");
        }

        QLocalSocket reconstructed;
        reconstructed.connectToServer(QString::fromStdString(endpoint), QLocalSocket::ReadWrite);
        require(reconstructed.waitForConnected(5000),
                "the reconstructed client reaches the adopted service");
        const auto reconstructed_hello =
            attach(reconstructed, fingerprint.toStdString(), session_id, epoch, false);
        require(reconstructed_hello.pid == terminal_child &&
                    reconstructed_hello.attachment.identity == hello.attachment.identity,
                "reconstruction preserves the terminal child and session identity");
        static_cast<void>(synchronize(reconstructed));
        send_text(reconstructed, reconstructed_hello.attachment, U'R');
        require(screen_text(wait_text(reconstructed, U'R')).find(U'R') != std::u32string::npos,
                "the adopted service still owns terminal input");
        detach(reconstructed);

        require(::kill(replacement_daemon, SIGTERM) == 0, "stop the replacement daemon");
        require(::waitpid(replacement_daemon, &status, 0) == replacement_daemon &&
                    WIFEXITED(status) && WEXITSTATUS(status) == 0,
                "the replacement daemon exits cleanly");
        replacement_daemon = -1;
        wait_gone(service_pid);
        wait_gone(static_cast<pid_t>(terminal_child));
        struct stat ownership_info{};
        require(::lstat((endpoint + ".supervisor-owner").c_str(), &ownership_info) != 0 &&
                    errno == ENOENT,
                "replacement-daemon stop removes service ownership");
        QFile state_file{directory.filePath(QStringLiteral("supervisor.json"))};
        require(state_file.open(QIODevice::ReadOnly), "read persisted state after stop");
        const auto state_document = QJsonDocument::fromJson(state_file.readAll());
        state_file.close();
        require(state_document.isObject() &&
                    state_document.object().value(QStringLiteral("version")).toInt() == 2,
                "reconstruction uses version-two state");
    } catch (...) {
        if (first_daemon > 0) {
            static_cast<void>(::kill(first_daemon, SIGKILL));
            int status = 0;
            static_cast<void>(::waitpid(first_daemon, &status, 0));
            first_daemon = -1;
        }
        if (replacement_daemon > 0) {
            static_cast<void>(::kill(replacement_daemon, SIGTERM));
            int status = 0;
            static_cast<void>(::waitpid(replacement_daemon, &status, 0));
            replacement_daemon = -1;
        }
        if (service_pid > 0) {
            static_cast<void>(::kill(service_pid, SIGKILL));
            wait_gone(service_pid);
            service_pid = -1;
        }
        if (terminal_child > 0)
            wait_gone(static_cast<pid_t>(terminal_child));
        terminal_child = 0;
        QDir::setCurrent(previous);
        throw;
    }
    QDir::setCurrent(previous);
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication application{argc, argv};
    try {
        failed_service_birth_never_reports_born();
        daemon_birth_and_real_service_clients();
        daemon_sigkill_reconstructs_the_live_service();
        std::cout << "supervisor-daemon: ok\n";
        return 0;
    } catch (const BindUnavailable&) {
        return 75;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
