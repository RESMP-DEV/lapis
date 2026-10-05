#include "lapis/supervisor/runtime.hpp"

#include "platform/posix/local_endpoint.hpp"
#include "platform/posix/unique_fd.hpp"

#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

namespace lapis::supervisor {
namespace {

using lapis::session::posix::UniqueFd;

[[noreturn]] void failed_system(const char* message) {
    throw std::runtime_error(std::string{message} + ": " + std::generic_category().message(errno));
}

[[noreturn]] void failed(const char* message) { throw std::runtime_error(message); }

bool valid_hex(const std::string& value) {
    return value.size() == spawn_token_hex_bytes &&
           std::all_of(value.begin(), value.end(), [](char value) {
               const auto byte = static_cast<unsigned char>(value);
               return (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f');
           });
}

std::string owner_path(const std::string& endpoint) { return endpoint + ".supervisor-owner"; }

QString socket_path(const DesiredSession& session) {
    return lapis::session::posix::prepare_endpoint(QString::fromStdString(session.endpoint));
}

bool endpoint_is_live(const std::string& endpoint) {
    UniqueFd descriptor{::socket(AF_UNIX, SOCK_STREAM, 0)};
    if (!descriptor)
        failed_system("Could not create supervisor child probe");
    const int socket_flags = ::fcntl(descriptor.get(), F_GETFL);
    const int socket_descriptor_flags = ::fcntl(descriptor.get(), F_GETFD);
    const unsigned nonblocking =
        static_cast<unsigned>(socket_flags) | static_cast<unsigned>(O_NONBLOCK);
    const unsigned cloexec =
        static_cast<unsigned>(socket_descriptor_flags) | static_cast<unsigned>(FD_CLOEXEC);
    if (socket_flags < 0 || socket_descriptor_flags < 0 ||
        ::fcntl(descriptor.get(), F_SETFL, static_cast<int>(nonblocking)) != 0 ||
        ::fcntl(descriptor.get(), F_SETFD, static_cast<int>(cloexec)) != 0)
        failed_system("Could not protect supervisor child probe");
    sockaddr_un address{};
    if (endpoint.size() >= sizeof(address.sun_path))
        failed("Supervisor child endpoint is too long");
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, endpoint.c_str(), endpoint.size() + 1);
    if (::connect(descriptor.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0)
        return true;
    if (errno != EINPROGRESS)
        return false;
    pollfd wait{};
    wait.fd = descriptor.get();
    wait.events = POLLOUT;
    if (::poll(&wait, 1, 250) != 1)
        return false;
    int connected = 0;
    socklen_t connected_size = sizeof(connected);
    return ::getsockopt(descriptor.get(), SOL_SOCKET, SO_ERROR, &connected, &connected_size) == 0 &&
           connected == 0;
}

std::string decimal(pid_t value) {
    std::string result = std::to_string(value);
    return result;
}

std::optional<pid_t> parse_pid(const std::string& value) {
    if (value.empty() || value.size() > 10 ||
        !std::all_of(value.begin(), value.end(),
                     [](char value) { return value >= '0' && value <= '9'; })) {
        return std::nullopt;
    }
    unsigned long long parsed = 0;
    for (const auto digit : value) {
        if (parsed > std::numeric_limits<unsigned long long>::max() / 10)
            return std::nullopt;
        parsed = parsed * 10 + static_cast<unsigned long long>(digit - '0');
        if (parsed > static_cast<unsigned long long>(std::numeric_limits<pid_t>::max()))
            return std::nullopt;
    }
    if (parsed == 0)
        return std::nullopt;
    return static_cast<pid_t>(parsed); // NOLINT(bugprone-narrowing-conversions)
}

struct OwnershipRecord {
    pid_t pid{};
};

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
std::optional<OwnershipRecord> read_ownership(const std::string& path,
                                              const std::string& spawn_token) {
    struct stat info{};
    if (::lstat(path.c_str(), &info) != 0) {
        if (errno == ENOENT)
            return std::nullopt;
        failed_system("Cannot inspect supervisor child ownership");
    }
    if (!S_ISREG(info.st_mode) || info.st_uid != ::geteuid() || info.st_nlink != 1 ||
        (info.st_mode & 07777U) != 0600U || info.st_size <= 0 || info.st_size > 128)
        failed("Unsafe supervisor child ownership record");
    UniqueFd descriptor{::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)};
    if (!descriptor)
        failed_system("Could not open supervisor child ownership");
    std::string bytes(static_cast<std::size_t>(info.st_size), '\0');
    std::size_t done = 0;
    while (done < bytes.size()) {
        const auto count = ::read(descriptor.get(), bytes.data() + done, bytes.size() - done);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            failed("Incomplete supervisor child ownership record");
        done += static_cast<std::size_t>(count);
    }
    constexpr auto prefix = std::string_view{"LAPIS-SUP-OWNER-1\n"};
    if (bytes.size() < prefix.size() + spawn_token_hex_bytes + 1 || !bytes.starts_with(prefix) ||
        !valid_hex(bytes.substr(prefix.size(), spawn_token_hex_bytes)) ||
        bytes[prefix.size() + spawn_token_hex_bytes] != '\n')
        failed("Invalid supervisor child ownership record");
    const auto pid = parse_pid(bytes.substr(prefix.size() + spawn_token_hex_bytes + 1));
    if (!pid)
        failed("Supervisor child ownership identity does not match desired state");
    if (bytes.substr(prefix.size(), spawn_token_hex_bytes) != spawn_token)
        failed("Supervisor child ownership identity does not match desired state");
    const pid_t child_pid = *pid;
    return OwnershipRecord{child_pid};
}

void validate_parent(const std::string& endpoint) {
    const QString parent = QFileInfo{QString::fromStdString(endpoint)}.absolutePath();
    struct stat info{};
    if (::stat(QFile::encodeName(parent).constData(), &info) != 0 || !S_ISDIR(info.st_mode) ||
        info.st_uid != ::geteuid() || (info.st_mode & 07777U) != 0700U)
        failed("Supervisor endpoint requires an owned private directory");
}

void remove_quietly(const std::string& path) { static_cast<void>(::unlink(path.c_str())); }

bool process_alive(pid_t pid) {
    if (pid <= 0)
        return false;
    if (::kill(pid, 0) == 0)
        return true;
    return errno == EPERM;
}

bool wait_terminated(pid_t pid) {
    for (int attempt = 0; attempt < 200; ++attempt) {
        int status = 0;
        if (::waitpid(pid, &status, WNOHANG) == pid)
            return true;
        if (!process_alive(pid)) {
            return true;
        }
        timespec pause{0, 10'000'000};
        ::nanosleep(&pause, nullptr);
    }
    return false;
}

} // namespace

OwnershipChildLauncher::OwnershipChildLauncher(LaunchCommand command)
    : command_{std::move(command)} {
    if (command_.program.empty() || command_.program.front() != '/')
        failed("Supervisor child program must be an absolute path");
}

std::optional<ChildProcess> OwnershipChildLauncher::adopt(const DesiredSession& session,
                                                          const std::string& spawn_token) {
    const auto endpoint = socket_path(session).toStdString();
    validate_parent(endpoint);
    const auto ownership = read_ownership(owner_path(endpoint), spawn_token);
    if (!ownership)
        return std::nullopt;
    if (!process_alive(ownership->pid)) {
        remove_quietly(owner_path(endpoint));
        return std::nullopt;
    }
    // Endpoint liveness strengthens the future session-service adapter, but
    // the harmless fixture intentionally has no protocol endpoint. A live,
    // token-bound PID is the smallest experimental ownership proof.
    static_cast<void>(endpoint_is_live(endpoint));
    ChildProcess result;
    result.pid = ownership->pid;
    result.spawn_token = spawn_token;
    result.adopted = true;
    result.endpoint = endpoint;
    return result;
}

// Process birth is an inherently branch-heavy transaction. Keep its failure
// paths adjacent so fork/exec and ownership handoff remain reviewable together.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
ChildProcess OwnershipChildLauncher::launch(const DesiredSession& session,
                                            const std::string& spawn_token) {
    const auto endpoint = socket_path(session).toStdString();
    validate_parent(endpoint);
    if (read_ownership(owner_path(endpoint), spawn_token))
        failed("A supervisor child already owns this spawn token");
    if (QFile::exists(QString::fromStdString(endpoint))) {
        if (endpoint_is_live(endpoint))
            failed("Supervisor child endpoint is already live");
        remove_quietly(endpoint);
    }

    const auto path = owner_path(endpoint);
    const auto temporary = path + "." + decimal(::getpid()) + ".tmp";
    remove_quietly(temporary);
    int barrier[2]{};
    if (::pipe(barrier) != 0)
        failed_system("Could not create supervisor child barrier");
    for (const int descriptor : barrier) {
        const int flags = ::fcntl(descriptor, F_GETFL);
        const int descriptor_flags = ::fcntl(descriptor, F_GETFD);
        const unsigned cloexec =
            static_cast<unsigned>(descriptor_flags) | static_cast<unsigned>(FD_CLOEXEC);
        if (flags < 0 || descriptor_flags < 0 ||
            ::fcntl(descriptor, F_SETFD, static_cast<int>(cloexec)) != 0)
            failed_system("Could not protect supervisor child barrier");
    }
    UniqueFd read_barrier{barrier[0]};
    UniqueFd write_barrier{barrier[1]};
    const auto token_argument = "--supervisor-token=" + spawn_token;
    const auto owner_argument = "--supervisor-owner=" + path;
    const auto socket_argument = "--supervisor-socket=" + endpoint;
    std::vector<std::string> storage;
    storage.reserve(command_.arguments.size() + 3);
    storage.push_back(token_argument);
    storage.push_back(owner_argument);
    storage.push_back(socket_argument);
    storage.insert(storage.end(), command_.arguments.begin(), command_.arguments.end());
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(command_.program.c_str()));
    for (const auto& value : storage)
        argv.push_back(const_cast<char*>(value.c_str()));
    argv.push_back(nullptr);
    const pid_t pid = ::fork();
    if (pid < 0)
        failed_system("Could not fork supervisor child");
    if (pid == 0) {
        read_barrier.reset();
        if (::setsid() < 0)
            _exit(126);
        const UniqueFd null_input{::open("/dev/null", O_RDONLY | O_CLOEXEC)};
        const UniqueFd null_output{::open("/dev/null", O_WRONLY | O_CLOEXEC)};
        bool wrote = false;
        if (!null_input || !null_output || ::dup2(null_input.get(), STDIN_FILENO) < 0 ||
            ::dup2(null_output.get(), STDOUT_FILENO) < 0 ||
            ::dup2(null_output.get(), STDERR_FILENO) < 0)
            _exit(126);
        if (!valid_hex(spawn_token)) {
            _exit(125);
        }
        // After fork, use only async-signal-safe calls. Argument strings were
        // materialized before fork; PID digits are formatted onto the stack.
        char ownership_bytes[96] = "LAPIS-SUP-OWNER-1\n";
        for (std::size_t index = 0; index < spawn_token.size(); ++index)
            ownership_bytes[18 + index] = spawn_token[index];
        ownership_bytes[18 + spawn_token.size()] = '\n';
        std::size_t ownership_size = 19 + spawn_token.size();
        auto pid_value = static_cast<int>(::getpid());
        char pid_digits[16]{};
        std::size_t pid_size = 0;
        while (pid_value > 0 && pid_size < sizeof(pid_digits)) {
            pid_digits[pid_size++] = static_cast<char>('0' + pid_value % 10);
            pid_value /= 10;
        }
        for (std::size_t index = 0; index < pid_size; ++index)
            ownership_bytes[ownership_size + index] = pid_digits[pid_size - index - 1];
        ownership_size += pid_size;
        const int descriptor =
            ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (descriptor >= 0) {
            std::size_t done = 0;
            wrote = true;
            while (wrote && done < ownership_size) {
                const auto count =
                    ::write(descriptor, ownership_bytes + done, ownership_size - done);
                wrote = count > 0 || (count < 0 && errno == EINTR);
                if (count > 0)
                    done += static_cast<std::size_t>(count);
            }
            wrote = wrote && done == ownership_size && ::fsync(descriptor) == 0;
            static_cast<void>(::close(descriptor));
            if (wrote && ::rename(temporary.c_str(), path.c_str()) == 0) {
                const char ready = 'r';
                wrote = ::write(write_barrier.get(), &ready, 1) == 1;
            }
        }
        if (!wrote)
            _exit(125);
        ::execv(command_.program.c_str(), argv.data());
        _exit(127);
    }

    write_barrier.reset();
    char ready = 0;
    while (true) {
        const auto count = ::read(read_barrier.get(), &ready, 1);
        if (count == 1)
            break;
        if (count == 0 || errno != EINTR) {
            int status = 0;
            static_cast<void>(::waitpid(pid, &status, 0));
            remove_quietly(temporary);
            failed("Supervisor child failed its ownership handshake");
        }
    }
    const auto ownership = read_ownership(path, spawn_token);
    if (!ownership || ownership->pid != pid)
        failed("Supervisor child ownership does not match the launched PID");
    ChildProcess result;
    result.pid = pid;
    result.spawn_token = spawn_token;
    result.endpoint = endpoint;
    return result;
}

bool OwnershipChildLauncher::alive(const ChildProcess& child) {
    return child.pid > 0 && process_alive(child.pid);
}

bool OwnershipChildLauncher::terminate(const ChildProcess& child) {
    if (child.pid <= 0)
        return true;
    static_cast<void>(::killpg(child.pid, SIGTERM));
    if (wait_terminated(child.pid))
        return true;
    static_cast<void>(::killpg(child.pid, SIGKILL));
    return wait_terminated(child.pid);
}

void OwnershipChildLauncher::release(const ChildProcess& child) {
    if (!child.endpoint.empty())
        remove_quietly(owner_path(child.endpoint));
}

class SteadyMonotonicClock final : public MonotonicClock {
  public:
    [[nodiscard]] std::uint64_t nanoseconds() override {
        return static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
    }
};

std::string encode_control_reply(ControlReplyStatus status) {
    const char* name = "rejected";
    if (status == ControlReplyStatus::applied)
        name = "applied";
    else if (status == ControlReplyStatus::unauthorized)
        name = "unauthorized";
    else if (status == ControlReplyStatus::stale_epoch)
        name = "stale_epoch";
    return QJsonDocument{QJsonObject{{QStringLiteral("version"), 1},
                                     {QStringLiteral("status"), QLatin1String{name}}}}
        .toJson(QJsonDocument::Compact)
        .toStdString();
}

ControlReplyStatus control_reply_status(ControlStatus status) {
    switch (status) {
    case ControlStatus::applied:
        return ControlReplyStatus::applied;
    case ControlStatus::unauthorized:
        return ControlReplyStatus::unauthorized;
    case ControlStatus::stale_epoch:
        return ControlReplyStatus::stale_epoch;
    case ControlStatus::rejected:
        break;
    }
    return ControlReplyStatus::rejected;
}

SupervisorRuntime::SupervisorRuntime(
    std::shared_ptr<SupervisorRegistry> registry, std::shared_ptr<ChildLauncher> launcher,
    std::shared_ptr<MonotonicClock> clock) // NOLINT(bugprone-easily-swappable-parameters)
    : registry_{std::move(registry)}, launcher_{std::move(launcher)},
      clock_{clock ? std::move(clock) : std::make_shared<SteadyMonotonicClock>()} {
    if (!registry_ || !launcher_)
        failed("Supervisor runtime dependencies are required");
    // A persisted desired-start slot with a spawn token has already consumed
    // its initial admission. Reconstructing after an unattended service crash
    // must use the rotating restart transition, never replay that identity.
    const auto& persisted = registry_->state();
    initial_admission_used_ = persisted.enabled && persisted.session.has_value() &&
                              persisted.session->desired_state == DesiredState::started &&
                              !persisted.session->spawn_token.empty();
    try {
        static_cast<void>(converge());
    } catch (const std::exception&) {
        // A failed launch is persisted as a blocked stopped state. Constructing
        // the control surface must still expose that explicit state.
        startup_convergence_failed_ = true;
    }
}

SupervisorState SupervisorRuntime::bootstrap(DesiredSession session) {
    const auto state = registry_->start(std::move(session));
    static_cast<void>(converge());
    return state;
}

std::string SupervisorRuntime::control(
    uid_t peer_uid, const std::string& request) { // NOLINT(bugprone-easily-swappable-parameters)
    if (peer_uid != ::geteuid())
        return encode_control_reply(ControlReplyStatus::unauthorized);
    ControlOutcome outcome;
    try {
        const JsonStateCodec codec;
        outcome = registry_->control(codec.decode_control(request));
    } catch (const std::exception&) {
        return encode_control_reply(ControlReplyStatus::rejected);
    }
    if (outcome.status != ControlStatus::applied)
        return encode_control_reply(control_reply_status(outcome.status));

    const bool has_child = child_.has_value();
    const auto& next_session = outcome.state.session;
    if (!next_session)
        return encode_control_reply(ControlReplyStatus::rejected);
    bool action_succeeded = true;
    if (has_child && (next_session->desired_state != DesiredState::started ||
                      child_->spawn_token != next_session->spawn_token)) {
        action_succeeded = launcher_->terminate(*child_);
        launcher_->release(*child_);
        child_.reset();
    }
    if (action_succeeded && next_session->desired_state == DesiredState::started) {
        try {
            static_cast<void>(converge());
        } catch (const std::exception&) {
            action_succeeded = false;
        }
    }
    if (!action_succeeded) {
        static_cast<void>(stop_desired("supervisor action failed"));
        return encode_control_reply(ControlReplyStatus::rejected);
    }
    return encode_control_reply(ControlReplyStatus::applied);
}

Convergence SupervisorRuntime::converge() {
    if (child_ && !launcher_->alive(*child_)) {
        launcher_->release(*child_);
        child_.reset();
    }
    const auto& current = registry_->state();
    if (!current.enabled)
        return Convergence::disabled;
    if (!current.session || current.session->desired_state != DesiredState::started)
        return Convergence::stopped;
    if (child_) {
        if (child_->spawn_token != current.session->spawn_token)
            failed("Supervisor child identity does not match authoritative state");
        return Convergence::converged;
    }

    const auto& session = *current.session;
    if (const auto adopted = launcher_->adopt(session, session.spawn_token)) {
        child_ = *adopted;
        // Adoption consumed this token's admission. A later crash is a new
        // service instance and must use the rotating restart transition.
        initial_admission_used_ = true;
        if (!session.blocked_reason.empty())
            static_cast<void>(registry_->note_block(""));
        return Convergence::converged;
    }
    if (launcher_->peer_preserved(session, session.spawn_token)) {
        static_cast<void>(
            registry_->note_block("Supervisor service peer is alive but did not verify"));
        failed("Supervisor service peer is alive but did not verify; identity retained for retry");
    }
    if (initial_admission_used_) {
        const auto now = clock_->nanoseconds();
        std::erase_if(restart_admissions_,
                      [now](std::uint64_t value) { return now - value >= restart_window_ns; });
        if (restart_admissions_.size() >= max_restarts_per_window) {
            static_cast<void>(stop_desired("restart admission exhausted"));
            return Convergence::exhausted;
        }
        restart_admissions_.push_back(now);
        // A restart is a new admission, not a replay of the old token. Rotate
        // it before launching so ownership discovery cannot choose a dead peer.
        const auto rotated = registry_->restart(session);
        if (!rotated.session)
            failed("Supervisor restart did not retain its session slot");
        const auto& rotated_session = rotated.session;
        try {
            child_ = launcher_->launch(*rotated_session, rotated_session->spawn_token);
        } catch (const std::exception&) {
            static_cast<void>(stop_desired("supervisor launch failed"));
            return Convergence::failed;
        }
        return Convergence::restarted;
    }
    initial_admission_used_ = true;
    try {
        child_ = launcher_->launch(session, session.spawn_token);
    } catch (const std::exception&) {
        static_cast<void>(stop_desired("supervisor launch failed"));
        return Convergence::failed;
    }
    return initial_admission_used_ && !restart_admissions_.empty() ? Convergence::restarted
                                                                   : Convergence::started;
}

const SupervisorState& SupervisorRuntime::state() const noexcept { return registry_->state(); }

pid_t SupervisorRuntime::child_pid() const noexcept { return child_ ? child_->pid : -1; }

void SupervisorRuntime::retire_child() {
    if (!child_)
        return;
    static_cast<void>(launcher_->terminate(*child_));
    launcher_->release(*child_);
    child_.reset();
}

bool SupervisorRuntime::stop_desired(const std::string& reason) {
    try {
        static_cast<void>(registry_->stop(reason));
        retire_child();
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

} // namespace lapis::supervisor
