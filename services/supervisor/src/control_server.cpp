#include "lapis/supervisor/control_server.hpp"

#include "platform/posix/local_endpoint.hpp"
#include "platform/posix/unique_fd.hpp"

#include <QFile>
#include <QFileInfo>
#include <QString>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>
#include <vector>

#if defined(__APPLE__) || defined(__OpenBSD__) || defined(__FreeBSD__)
#define LAPIS_HAVE_GETPEEREID 1
#endif

#if defined(__linux__)
#define LAPIS_HAVE_SO_PEERCRED 1
#endif

namespace lapis::supervisor {
namespace {

using lapis::session::posix::UniqueFd;

constexpr std::size_t control_header_bytes = 4;

[[noreturn]] void failed_system(const char* message) {
    throw std::runtime_error(std::string{message} + ": " + std::generic_category().message(errno));
}

void failed(const char* message) { throw std::runtime_error(message); }

uint32_t big_endian_length(const unsigned char bytes[control_header_bytes]) {
    return (static_cast<uint32_t>(bytes[0]) << 24U) | (static_cast<uint32_t>(bytes[1]) << 16U) |
           (static_cast<uint32_t>(bytes[2]) << 8U) | static_cast<uint32_t>(bytes[3]);
}

void write_all(int descriptor,
               const std::string& bytes) { // NOLINT(bugprone-easily-swappable-parameters)
    std::size_t done = 0;
    while (done < bytes.size()) {
        const auto count = ::write(descriptor, bytes.data() + done, bytes.size() - done);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return;
        done += static_cast<std::size_t>(count);
    }
}

std::optional<uid_t> peer_uid(int descriptor) {
#if defined(LAPIS_HAVE_GETPEEREID)
    uid_t uid = static_cast<uid_t>(-1);
    gid_t gid = static_cast<gid_t>(-1);
    if (::getpeereid(descriptor, &uid, &gid) != 0)
        return std::nullopt;
    return uid;
#elif defined(LAPIS_HAVE_SO_PEERCRED)
    struct ucred credentials{};
    socklen_t size = sizeof(credentials);
    if (::getsockopt(descriptor, SOL_SOCKET, SO_PEERCRED, &credentials, &size) != 0 ||
        size != sizeof(credentials))
        return std::nullopt;
    return credentials.uid;
#else
    static_cast<void>(descriptor);
    return std::nullopt;
#endif
}

QString prepared_endpoint(const std::string& endpoint) {
    return lapis::session::posix::prepare_endpoint(QString::fromStdString(endpoint));
}

void make_nonblocking_cloexec(int descriptor) {
    const int flags = ::fcntl(descriptor, F_GETFL);
    const int descriptor_flags = ::fcntl(descriptor, F_GETFD);
    const unsigned nonblocking = static_cast<unsigned>(flags) | static_cast<unsigned>(O_NONBLOCK);
    const unsigned cloexec =
        static_cast<unsigned>(descriptor_flags) | static_cast<unsigned>(FD_CLOEXEC);
    if (flags < 0 || descriptor_flags < 0 ||
        ::fcntl(descriptor, F_SETFL, static_cast<int>(nonblocking)) != 0 ||
        ::fcntl(descriptor, F_SETFD, static_cast<int>(cloexec)) != 0)
        failed_system("Could not protect supervisor control descriptor");
}

struct ControlClient {
    UniqueFd descriptor;
    uid_t uid{};
    std::string buffer;
};

enum class ReceiveResult : std::uint8_t { incomplete, ready, closed };

ReceiveResult receive_header(ControlClient& client) {
    while (client.buffer.size() < control_header_bytes) {
        char header[control_header_bytes]{};
        const auto count =
            ::read(client.descriptor.get(), header, control_header_bytes - client.buffer.size());
        if (count == 0)
            return ReceiveResult::closed;
        if (count < 0)
            return (errno == EAGAIN || errno == EWOULDBLOCK) ? ReceiveResult::incomplete
                                                             : ReceiveResult::closed;
        client.buffer.append(header, static_cast<std::size_t>(count));
    }

    unsigned char lengths[control_header_bytes]{};
    std::memcpy(lengths, client.buffer.data(), control_header_bytes);
    const auto length = big_endian_length(lengths);
    if (length > max_control_bytes)
        return ReceiveResult::closed;
    client.buffer.clear();
    client.buffer.append(reinterpret_cast<const char*>(lengths), control_header_bytes);
    return ReceiveResult::incomplete;
}

ReceiveResult receive_payload(ControlClient& client) {
    const auto expected =
        big_endian_length(reinterpret_cast<const unsigned char*>(client.buffer.data()));
    if (client.buffer.size() >= control_header_bytes + expected)
        return ReceiveResult::ready;
    std::string chunk(
        std::min<std::size_t>(max_control_bytes + control_header_bytes - client.buffer.size(),
                              static_cast<std::size_t>(4096)),
        '\0');
    const auto count = ::read(client.descriptor.get(), chunk.data(), chunk.size());
    if (count == 0)
        return ReceiveResult::closed;
    if (count < 0)
        return (errno == EAGAIN || errno == EWOULDBLOCK) ? ReceiveResult::incomplete
                                                         : ReceiveResult::closed;
    client.buffer.append(chunk.data(), static_cast<std::size_t>(count));
    if (big_endian_length(reinterpret_cast<const unsigned char*>(client.buffer.data())) !=
        client.buffer.size() - control_header_bytes)
        return ReceiveResult::incomplete;
    return ReceiveResult::ready;
}

bool process_client(const ControlHandler& handler, ControlClient& client) {
    const auto header = receive_header(client);
    if (header == ReceiveResult::closed)
        return false;
    if (header == ReceiveResult::incomplete && client.buffer.size() < control_header_bytes)
        return true;
    const auto payload_result = receive_payload(client);
    if (payload_result != ReceiveResult::ready)
        return true;
    const std::string payload = client.buffer.substr(control_header_bytes);
    write_all(client.descriptor.get(), handler(client.uid, payload));
    return false;
}

void accept_clients(const UniqueFd& listener, std::vector<ControlClient>& clients) {
    while (clients.size() < max_control_clients) {
        const int accepted = ::accept(listener.get(), nullptr, nullptr);
        if (accepted < 0) {
            if (errno == EINTR)
                continue;
            return;
        }
        make_nonblocking_cloexec(accepted);
        UniqueFd descriptor{accepted};
        const auto uid = peer_uid(descriptor.get());
        if (!uid || *uid != ::geteuid())
            continue;
        clients.push_back({std::move(descriptor), *uid, {}});
    }
}

} // namespace

struct LocalControlServer::State {
    std::string endpoint;
    ControlHandler handler;
    UniqueFd listener;
    std::vector<ControlClient> clients;

    void close_clients() { clients.clear(); }
};

LocalControlServer::LocalControlServer(
    std::string endpoint, ControlHandler handler) // NOLINT(bugprone-easily-swappable-parameters)
    : state_{std::make_unique<State>()} {
    if (endpoint.empty() || endpoint.front() != '/' || endpoint.size() >= 104)
        failed("Supervisor control endpoint must be an absolute bounded path");
    const auto prepared = prepared_endpoint(endpoint);
    state_->endpoint = prepared.toStdString();
    state_->handler = std::move(handler);
    if (!state_->handler)
        failed("Supervisor control handler is required");

    if (QFile::exists(prepared)) {
        UniqueFd probe{::socket(AF_UNIX, SOCK_STREAM, 0)};
        if (probe)
            make_nonblocking_cloexec(probe.get());
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        if (state_->endpoint.size() >= sizeof(address.sun_path))
            failed("Supervisor control endpoint is too long");
        std::memcpy(address.sun_path, state_->endpoint.c_str(), state_->endpoint.size() + 1);
        if (probe &&
            ::connect(probe.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0)
            failed("Supervisor control endpoint is already in use");
        if (!QFile::remove(prepared))
            failed_system("Could not remove the stale supervisor control endpoint");
    }

    state_->listener = UniqueFd{::socket(AF_UNIX, SOCK_STREAM, 0)};
    if (state_->listener)
        make_nonblocking_cloexec(state_->listener.get());
    if (!state_->listener)
        failed_system("Could not create supervisor control socket");
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, state_->endpoint.c_str(), state_->endpoint.size() + 1);
    if (::bind(state_->listener.get(), reinterpret_cast<const sockaddr*>(&address),
               sizeof(address)) != 0)
        failed_system("Could not bind supervisor control socket");
    if (::chmod(state_->endpoint.c_str(), 0600) != 0)
        failed_system("Could not protect supervisor control socket");
    if (::listen(state_->listener.get(), max_control_clients) != 0)
        failed_system("Could not listen on supervisor control socket");
    struct stat info{};
    if (::lstat(state_->endpoint.c_str(), &info) != 0 || !S_ISSOCK(info.st_mode) ||
        info.st_uid != ::geteuid() || (info.st_mode & 07777U) != 0600U)
        failed("Bound supervisor control socket is not private");
}

LocalControlServer::~LocalControlServer() {
    if (state_) {
        state_->close_clients();
        state_->listener.reset();
        static_cast<void>(::unlink(state_->endpoint.c_str()));
    }
}

void LocalControlServer::poll_once(int timeout_ms) {
    std::vector<pollfd> waits;
    waits.reserve(state_->clients.size() + 1);
    waits.push_back({state_->listener.get(), POLLIN, 0});
    for (const auto& client : state_->clients)
        waits.push_back({client.descriptor.get(), POLLIN, 0});
    int ready = ::poll(waits.data(), static_cast<nfds_t>(waits.size()), timeout_ms);
    if (ready < 0) {
        if (errno == EINTR)
            return;
        failed_system("Could not poll supervisor control socket");
    }
    if (ready == 0)
        return;

    if (waits.front().revents != 0) {
        static_cast<void>(ready);
        accept_clients(state_->listener, state_->clients);
    }

    for (auto client = state_->clients.begin(); client != state_->clients.end();) {
        client = process_client(state_->handler, *client) ? std::next(client)
                                                          : state_->clients.erase(client);
    }
}

} // namespace lapis::supervisor
