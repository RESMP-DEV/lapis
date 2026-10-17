#include "lapis/supervisor/client.hpp"
#include "lapis/supervisor/control_server.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

namespace {
using namespace lapis::supervisor;

void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error{message};
}

std::string repeat(char value, std::size_t count) { return std::string(count, value); }

SupervisorState empty_state() {
    SupervisorState state;
    state.instance_epoch = repeat('4', epoch_hex_bytes);
    return state;
}

SupervisorState stopped_state() {
    SupervisorState state = empty_state();
    state.enabled = true;
    DesiredSession session;
    session.endpoint = "/tmp/lapis-session.sock";
    session.fingerprint = repeat('5', fingerprint_hex_bytes);
    session.identity = {repeat('6', identity_hex_bytes), repeat('7', identity_hex_bytes)};
    session.spawn_token = repeat('8', spawn_token_hex_bytes);
    session.blocked_reason = "stopped for test";
    DesiredSessionLaunch launch;
    launch.program = "/bin/cat";
    launch.working_directory = "/tmp";
    session.launch = launch;
    state.session = session;
    return state;
}

QJsonObject json(const std::string& bytes) {
    const auto document = QJsonDocument::fromJson(QByteArray::fromStdString(bytes));
    require(document.isObject(), "control JSON is an object");
    return document.object();
}

void write_private_state(const QDir& directory, const SupervisorState& state) {
    directory.mkpath(QStringLiteral("."));
    QFile file{directory.filePath(QStringLiteral("supervisor.json"))};
    require(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "write test state");
    const auto bytes = JsonStateCodec{}.encode(state);
    file.write(bytes.data(), static_cast<qsizetype>(bytes.size()));
    file.close();
    require(QFile::setPermissions(file.fileName(), QFile::ReadOwner | QFile::WriteOwner) &&
                QFile::setPermissions(directory.path(),
                                      QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner),
            "make test state private");
}

void protocol_round_trip() {
    const auto request = json(encode_status_request());
    require(request.size() == 2 && request.value(QStringLiteral("version")).toInt() == 1 &&
                request.value(QStringLiteral("action")).toString() == QLatin1String("status"),
            "status request has the exact versioned action");

    const auto empty = decode_status_reply(encode_status_reply(empty_state(), false));
    require(empty.schema_version == 2 && empty.slot == "empty" && !empty.enabled &&
                !empty.service_live && empty.reconstructable && empty.endpoint.empty(),
            "empty status reports no service");

    const auto stopped = decode_status_reply(encode_status_reply(stopped_state(), false));
    require(stopped.slot == "stopped" && stopped.enabled && !stopped.service_live &&
                stopped.reconstructable && stopped.blocked_reason == "stopped for test" &&
                stopped.endpoint.empty(),
            "stopped status keeps its disposition without exposing an endpoint");

    auto started = stopped_state();
    started.session->desired_state = DesiredState::started;
    const auto live = decode_status_reply(encode_status_reply(started, true));
    require(live.service_live && live.endpoint == "/tmp/lapis-session.sock",
            "live status exposes only its service endpoint");
    bool refused_inconsistent = false;
    try {
        auto inconsistent = json(encode_status_reply(empty_state(), false));
        inconsistent.insert(QStringLiteral("enabled"), true);
        static_cast<void>(decode_status_reply(
            QJsonDocument(inconsistent).toJson(QJsonDocument::Compact).toStdString()));
    } catch (const std::exception&) {
        refused_inconsistent = true;
    }
    require(refused_inconsistent, "an empty slot cannot claim enabled or live state");

    const auto stop = json(encode_stop_request(stopped_state()));
    require(stop.size() == 5 &&
                stop.value(QStringLiteral("action")).toString() == QLatin1String("stop") &&
                stop.value(QStringLiteral("supervisor_epoch")).toString().toStdString() ==
                    repeat('4', epoch_hex_bytes) &&
                stop.value(QStringLiteral("token")).toString().toStdString() ==
                    repeat('8', spawn_token_hex_bytes) &&
                stop.value(QStringLiteral("session")).isNull(),
            "stop request carries exact supervisor identity and token");
}

void private_state_loading() {
    QTemporaryDir directory{QStringLiteral("/tmp/lapis-supervisor-state-XXXXXX")};
    require(directory.isValid(), "create state directory");
    const QDir root{QFileInfo(directory.path()).canonicalFilePath()};
    const auto expected = stopped_state();
    write_private_state(root, expected);
    require(read_control_state(root.path().toStdString()) == expected,
            "private state loads and decodes");

    auto legacy = expected;
    legacy.version = 1;
    legacy.session->launch.reset();
    write_private_state(root, legacy);
    bool refused_legacy = false;
    try {
        static_cast<void>(read_control_state(root.path().toStdString()));
    } catch (const std::exception&) {
        refused_legacy = true;
    }
    require(refused_legacy, "the stop client refuses version-one state");
    write_private_state(root, expected);

    QFile::setPermissions(root.path(), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner |
                                           QFile::ReadGroup);
    bool refused = false;
    try {
        static_cast<void>(read_control_state(root.path().toStdString()));
    } catch (const std::exception&) {
        refused = true;
    }
    require(refused, "a non-private state directory is refused");
    QFile::setPermissions(root.path(), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
}

void framed_request_round_trip() {
    QTemporaryDir directory{QStringLiteral("/tmp/lapis-supervisor-client-XXXXXX")};
    require(directory.isValid(), "create client directory");
    const std::string endpoint = directory.filePath(QStringLiteral("control.sock")).toStdString();
    SupervisorState empty;
    empty.instance_epoch = repeat('4', epoch_hex_bytes);
    LocalControlServer server{
        endpoint, [&](uid_t, const std::string& request) {
            return request == encode_status_request()
                       ? encode_status_reply(empty, false)
                       : QJsonDocument{QJsonObject{
                                           {QStringLiteral("status"), QStringLiteral("rejected")}}}
                             .toJson(QJsonDocument::Compact)
                             .toStdString();
        }};
    std::thread poll{[&server] { server.poll_once(2000); }};

    const int client = ::socket(AF_UNIX, SOCK_STREAM, 0);
    require(client >= 0, "create the raw control client");
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    require(endpoint.size() < sizeof(address.sun_path), "raw control endpoint length");
    std::memcpy(address.sun_path, endpoint.c_str(), endpoint.size() + 1);
    require(::connect(client, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0,
            "connect the raw control client");
    const auto request = encode_status_request();
    const auto length = static_cast<std::uint32_t>(request.size());
    const std::array<char, 4> header{
        static_cast<char>((length >> 24U) & 0xffU),
        static_cast<char>((length >> 16U) & 0xffU),
        static_cast<char>((length >> 8U) & 0xffU),
        static_cast<char>(length & 0xffU),
    };
    require(::write(client, header.data(), header.size()) == static_cast<ssize_t>(header.size()),
            "write the raw control header");
    require(::write(client, request.data(), request.size()) == static_cast<ssize_t>(request.size()),
            "write the raw control request");
    std::array<char, 4> response_header{};
    for (std::size_t done = 0; done < response_header.size();) {
        pollfd wait{client, POLLIN, 0};
        require(::poll(&wait, 1, 2000) == 1, "wait for the raw control header");
        const auto count =
            ::read(client, response_header.data() + done, response_header.size() - done);
        require(count > 0, "read the raw control header");
        done += static_cast<std::size_t>(count);
    }
    const auto response_length =
        (static_cast<std::uint32_t>(static_cast<unsigned char>(response_header[0])) << 24U) |
        (static_cast<std::uint32_t>(static_cast<unsigned char>(response_header[1])) << 16U) |
        (static_cast<std::uint32_t>(static_cast<unsigned char>(response_header[2])) << 8U) |
        static_cast<std::uint32_t>(static_cast<unsigned char>(response_header[3]));
    require(response_length <= max_control_bytes, "raw control reply is bounded");
    std::string response(response_length, '\0');
    for (std::size_t done = 0; done < response.size();) {
        pollfd wait{client, POLLIN, 0};
        require(::poll(&wait, 1, 2000) == 1, "wait for the raw control reply");
        const auto count = ::read(client, response.data() + done, response.size() - done);
        require(count > 0, "read the raw control reply");
        done += static_cast<std::size_t>(count);
    }
    static_cast<void>(::close(client));
    poll.join();
    require(decode_status_reply(response).slot == "empty", "framed request returns status");
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication application{argc, argv};
    try {
        protocol_round_trip();
        private_state_loading();
        framed_request_round_trip();
        std::cout << "supervisor-client-unit: ok\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
