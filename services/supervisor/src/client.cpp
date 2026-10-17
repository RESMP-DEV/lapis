#include "lapis/supervisor/client.hpp"

#include "platform/posix/unique_fd.hpp"

#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLocalSocket>
#include <QString>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace lapis::supervisor {
namespace {

[[noreturn]] void invalid(const char* message) { throw std::runtime_error(message); }

[[noreturn]] void invalid_system(const char* message) {
    throw std::runtime_error(std::string{message} + ": " + std::strerror(errno));
}

QJsonObject object(const std::string& bytes, const char* message) {
    if (bytes.empty() || bytes.size() > max_control_bytes)
        invalid(message);
    const auto document = QJsonDocument::fromJson(QByteArray::fromStdString(bytes));
    if (!document.isObject())
        invalid(message);
    return document.object();
}

void require_exact(const QJsonObject& root, std::initializer_list<QString> names,
                   const char* message) {
    if (root.size() != static_cast<qsizetype>(names.size()))
        invalid(message);
    for (const auto& name : names) {
        if (!root.contains(name))
            invalid(message);
    }
}

std::string json_bytes(const QJsonObject& root) {
    const auto bytes = QJsonDocument(root).toJson(QJsonDocument::Compact);
    return std::string{bytes.constData(), static_cast<std::size_t>(bytes.size())};
}

std::string slot_name(const SupervisorState& state) {
    if (!state.session)
        return "empty";
    return state.session->desired_state == DesiredState::started ? "started" : "stopped";
}

void valid_state_file(const struct stat& info) {
    if (!S_ISREG(info.st_mode) || info.st_uid != ::geteuid() || (info.st_mode & 07777U) != 0600U ||
        info.st_nlink != 1)
        invalid("Supervisor state is not a private regular file");
}

} // namespace

std::string encode_status_request() {
    return json_bytes(QJsonObject{{QStringLiteral("version"), 1},
                                  {QStringLiteral("action"), QStringLiteral("status")}});
}

std::string encode_status_reply(const SupervisorState& state, bool service_live) {
    const auto& session = state.session;
    return json_bytes(QJsonObject{
        {QStringLiteral("version"), 1},
        {QStringLiteral("status"), QStringLiteral("ok")},
        {QStringLiteral("schema_version"), state.version},
        {QStringLiteral("instance_epoch"), QString::fromStdString(state.instance_epoch)},
        {QStringLiteral("slot"), QString::fromStdString(slot_name(state))},
        {QStringLiteral("enabled"), state.enabled},
        {QStringLiteral("service_live"), service_live},
        {QStringLiteral("blocked_reason"),
         session ? QString::fromStdString(session->blocked_reason) : QString{}},
        {QStringLiteral("endpoint"), service_live && session
                                         ? QJsonValue{QString::fromStdString(session->endpoint)}
                                         : QJsonValue::Null},
        {QStringLiteral("reconstructable"),
         !session || (state.version == 2 && session->launch.has_value())},
    });
}

std::string encode_stop_request(const SupervisorState& state) {
    if (!state.session || state.session->spawn_token.empty())
        invalid("Supervisor state has no authenticated stop token");
    return json_bytes(QJsonObject{
        {QStringLiteral("version"), 1},
        {QStringLiteral("action"), QStringLiteral("stop")},
        {QStringLiteral("supervisor_epoch"), QString::fromStdString(state.instance_epoch)},
        {QStringLiteral("token"), QString::fromStdString(state.session->spawn_token)},
        {QStringLiteral("session"), QJsonValue::Null},
    });
}

SupervisorStatus decode_status_reply(const std::string& bytes) {
    constexpr auto* message = "Invalid supervisor status reply";
    const auto root = object(bytes, message);
    require_exact(root,
                  {QStringLiteral("version"), QStringLiteral("status"),
                   QStringLiteral("schema_version"), QStringLiteral("instance_epoch"),
                   QStringLiteral("slot"), QStringLiteral("enabled"),
                   QStringLiteral("service_live"), QStringLiteral("blocked_reason"),
                   QStringLiteral("endpoint"), QStringLiteral("reconstructable")},
                  message);
    if (root.value(QStringLiteral("version")).toInt() != 1 ||
        root.value(QStringLiteral("status")).toString() != QLatin1String("ok"))
        invalid(message);

    SupervisorStatus result;
    const auto version = root.value(QStringLiteral("schema_version")).toDouble();
    if (version != 1.0 && version != 2.0)
        invalid(message);
    result.schema_version = static_cast<int>(version);
    result.instance_epoch = root.value(QStringLiteral("instance_epoch")).toString().toStdString();
    result.slot = root.value(QStringLiteral("slot")).toString().toStdString();
    result.blocked_reason = root.value(QStringLiteral("blocked_reason")).toString().toStdString();
    result.endpoint = root.value(QStringLiteral("endpoint")).toString().toStdString();
    if (!valid_hex(result.instance_epoch, epoch_hex_bytes) ||
        (result.slot != "empty" && result.slot != "started" && result.slot != "stopped"))
        invalid(message);
    const auto enabled = root.value(QStringLiteral("enabled"));
    const auto service_live = root.value(QStringLiteral("service_live"));
    const auto reconstructable = root.value(QStringLiteral("reconstructable"));
    if (!enabled.isBool() || !service_live.isBool() || !reconstructable.isBool())
        invalid(message);
    result.enabled = enabled.toBool();
    result.service_live = service_live.toBool();
    result.reconstructable = reconstructable.toBool();
    if ((result.slot == "empty" &&
         (result.enabled || result.service_live || !result.endpoint.empty())) ||
        (result.service_live && result.slot != "started") ||
        (!result.endpoint.empty() && !result.service_live))
        invalid(message);
    return result;
}

SupervisorState read_control_state(const std::string& directory) {
    if (directory.empty() || directory.front() != '/')
        invalid("Supervisor state directory must be absolute");
    struct stat directory_info{};
    if (::lstat(directory.c_str(), &directory_info) != 0)
        invalid_system("Cannot inspect supervisor state directory");
    if (!S_ISDIR(directory_info.st_mode) || directory_info.st_uid != ::geteuid() ||
        (directory_info.st_mode & 07777U) != 0700U)
        invalid("Supervisor state directory must be private to its owner");

    const std::string path = directory + "/supervisor.json";
    struct stat info{};
    const lapis::session::posix::UniqueFd descriptor{
        ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW)};
    if (!descriptor)
        invalid_system("Could not open supervisor state");
    if (::fstat(descriptor.get(), &info) != 0)
        invalid_system("Cannot inspect supervisor state");
    valid_state_file(info);
    if (info.st_size <= 0 || static_cast<unsigned long long>(info.st_size) > max_state_bytes)
        invalid("Supervisor state is outside its persistence bound");
    std::string bytes(static_cast<std::size_t>(info.st_size), '\0');
    std::size_t done = 0;
    while (done < bytes.size()) {
        const auto count = ::read(descriptor.get(), bytes.data() + done, bytes.size() - done);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            invalid("Could not read the complete supervisor state");
        done += static_cast<std::size_t>(count);
    }
    const JsonStateCodec codec;
    const auto state = codec.decode(bytes);
    if (!valid_state(state) || state.version != 2)
        invalid("Supervisor state is invalid");
    return state;
}

std::string send_control_request(const std::string& endpoint, const std::string& request) {
    if (endpoint.empty() || endpoint.front() != '/')
        invalid("Supervisor control endpoint must be absolute");
    if (request.empty() || request.size() > max_control_bytes)
        invalid("Supervisor control request is outside its bound");

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{
                                                                 supervisor_client_timeout_ms,
                                                             };
    const auto remaining_milliseconds = [&] {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        return static_cast<int>(std::max<std::int64_t>(0, remaining.count()));
    };

    QLocalSocket socket;
    socket.connectToServer(QString::fromStdString(endpoint));
    if (!socket.waitForConnected(remaining_milliseconds()))
        invalid("Could not connect to the supervisor control endpoint");

    const auto length = static_cast<std::uint32_t>(request.size());
    QByteArray framed;
    for (const auto shift : {24U, 16U, 8U, 0U}) {
        framed.append(static_cast<char>((length >> shift) & 0xffU));
    }
    framed.append(request.data(), static_cast<qsizetype>(request.size()));
    // A stream socket may accept only part of one request; Qt keeps the rest
    // queued, so a partial write is progress and the drain below completes it.
    const auto written = socket.write(framed);
    if (written < 0 || !socket.flush())
        invalid("Could not send the supervisor control request");
    while (socket.bytesToWrite() != 0) {
        if (!socket.waitForBytesWritten(remaining_milliseconds()))
            invalid("Could not send the supervisor control request");
    }

    QByteArray response;
    while (true) {
        while (response.size() < 4) {
            if (!socket.waitForReadyRead(remaining_milliseconds()))
                invalid("Supervisor status reply timed out");
            response += socket.readAll();
        }
        const auto payload_size =
            static_cast<std::uint32_t>((static_cast<unsigned char>(response[0]) << 24U) |
                                       (static_cast<unsigned char>(response[1]) << 16U) |
                                       (static_cast<unsigned char>(response[2]) << 8U) |
                                       static_cast<unsigned char>(response[3]));
        if (payload_size > max_control_bytes)
            invalid("Supervisor control reply is outside its bound");
        const auto expected = static_cast<qsizetype>(payload_size + 4);
        while (response.size() < expected) {
            if (!socket.waitForReadyRead(remaining_milliseconds()))
                invalid("Supervisor control reply timed out");
            response += socket.readAll();
        }
        if (response.size() != expected)
            invalid("Supervisor control reply has trailing bytes");
        return response.mid(4).toStdString();
    }
}

} // namespace lapis::supervisor
