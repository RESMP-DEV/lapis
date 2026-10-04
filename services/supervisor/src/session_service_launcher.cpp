#include "lapis/supervisor/session_service_launcher.hpp"

#include "launch_spec.hpp"
#include "platform/posix/local_endpoint.hpp"
#include "platform/posix/unique_fd.hpp"
#include "transport/local_protocol.hpp"

#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QLocalSocket>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

#if defined(__APPLE__)
#include <libproc.h>
#endif

namespace lapis::supervisor {
namespace {
using lapis::session::LaunchSpec;
using lapis::session::posix::UniqueFd;
using AgentMode = lapis::session::AgentMode;
namespace wire = lapis::session::wire;

[[noreturn]] void failed(const char* message) { throw std::runtime_error(message); }
[[noreturn]] void failed_system(const char* message) {
    throw std::runtime_error(std::string{message} + ": " + std::generic_category().message(errno));
}

bool hex(const std::string& value, std::size_t length) { return valid_hex(value, length); }

std::string owner_path(const std::string& endpoint) { return endpoint + ".supervisor-owner"; }

void remove_quietly(const std::string& path) { static_cast<void>(::unlink(path.c_str())); }

bool process_alive(pid_t pid) {
    if (pid <= 0)
        return false;
    if (::kill(pid, 0) == 0)
        return true;
    return errno == EPERM;
}

bool wait_terminated(pid_t pid) {
    for (int attempt = 0; attempt < 300; ++attempt) {
        int status = 0;
        if (::waitpid(pid, &status, WNOHANG) == pid)
            return true;
        if (!process_alive(pid))
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

std::optional<pid_t> parse_pid(const std::string& value) {
    if (value.empty() || value.size() > 10 ||
        !std::all_of(value.begin(), value.end(),
                     [](char item) { return item >= '0' && item <= '9'; }))
        return std::nullopt;
    unsigned long long parsed = 0;
    for (const char item : value) {
        if (parsed > std::numeric_limits<unsigned long long>::max() / 10)
            return std::nullopt;
        parsed = parsed * 10 + static_cast<unsigned long long>(item - '0');
        if (parsed > static_cast<unsigned long long>(std::numeric_limits<pid_t>::max()))
            return std::nullopt;
    }
    return parsed == 0 ? std::nullopt
                       : std::optional{
                             static_cast<pid_t>(parsed)}; // NOLINT(bugprone-narrowing-conversions)
}

std::optional<pid_t> read_ownership(const std::string& path, const std::string& spawn_token) {
    struct stat info{};
    if (::lstat(path.c_str(), &info) != 0) {
        if (errno == ENOENT)
            return std::nullopt;
        failed_system("Cannot inspect supervisor service ownership");
    }
    if (!S_ISREG(info.st_mode) || info.st_uid != ::geteuid() || info.st_nlink != 1 ||
        (info.st_mode & 07777U) != 0600U || info.st_size <= 0 || info.st_size > 128)
        failed("Unsafe supervisor service ownership record");
    UniqueFd descriptor{::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)};
    if (!descriptor)
        failed_system("Could not open supervisor service ownership");
    std::string bytes(static_cast<std::size_t>(info.st_size), '\0');
    std::size_t done = 0;
    while (done < bytes.size()) {
        const auto count = ::read(descriptor.get(), bytes.data() + done, bytes.size() - done);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            failed("Incomplete supervisor service ownership record");
        done += static_cast<std::size_t>(count);
    }
    constexpr auto prefix = std::string_view{"LAPIS-SUP-OWNER-1\n"};
    if (bytes.size() < prefix.size() + spawn_token_hex_bytes + 1 || !bytes.starts_with(prefix) ||
        !hex(bytes.substr(prefix.size(), spawn_token_hex_bytes), spawn_token_hex_bytes) ||
        bytes[prefix.size() + spawn_token_hex_bytes] != '\n')
        failed("Invalid supervisor service ownership record");
    if (bytes.substr(prefix.size(), spawn_token_hex_bytes) != spawn_token)
        failed("Supervisor service ownership identity does not match authoritative state");
    return parse_pid(bytes.substr(prefix.size() + spawn_token_hex_bytes + 1));
}

bool process_runs_program(pid_t pid, const QString& program) {
    const std::string expected = QFileInfo{program}.canonicalFilePath().toStdString();
    if (expected.empty())
        return false;
    std::string actual;
#if defined(__APPLE__)
    std::vector<char> path(PROC_PIDPATHINFO_MAXSIZE);
    const auto size = ::proc_pidpath(pid, path.data(), static_cast<uint32_t>(path.size()));
    if (size > 0)
        actual.assign(path.data(), static_cast<std::size_t>(size));
#elif defined(__linux__)
    std::array<char, 4096> path{};
    const auto size =
        ::readlink(("/proc/" + std::to_string(pid) + "/exe").c_str(), path.data(), path.size() - 1);
    if (size > 0)
        actual.assign(path.data(), static_cast<std::size_t>(size));
#else
#error "Supervisor service adoption requires macOS proc_pidpath or Linux /proc executable discovery"
#endif
    return !actual.empty() && actual == expected;
}

QString socket_path(const DesiredSession& session) {
    return lapis::session::posix::prepare_endpoint(QString::fromStdString(session.endpoint));
}

void validate_parent(const QString& endpoint) {
    const QString parent = QFileInfo{endpoint}.absolutePath();
    struct stat info{};
    if (::stat(QFile::encodeName(parent).constData(), &info) != 0 || !S_ISDIR(info.st_mode) ||
        info.st_uid != ::geteuid() || (info.st_mode & 07777U) != 0700U)
        failed("Supervisor service endpoint requires an owned private directory");
}

QByteArray expected_fingerprint(const SessionServiceLaunch& launch) {
    LaunchSpec service_child;
    service_child.program = launch.agent_program;
    service_child.arguments = launch.agent_arguments;
    service_child.directory = launch.working_directory;
    service_child.size = {static_cast<std::uint16_t>(launch.columns),
                          static_cast<std::uint16_t>(launch.rows)};
    service_child.agent = launch.codex    ? AgentMode::codex
                          : launch.claude ? AgentMode::claude
                                          : AgentMode::terminal;
    return lapis::session::launch_fingerprint(service_child);
}

wire::SessionIdentity wire_identity(const DesiredSession& session) {
    return {QByteArray::fromHex(QByteArray::fromStdString(session.identity.session_id)),
            QByteArray::fromHex(QByteArray::fromStdString(session.identity.epoch))};
}
} // namespace

SessionServiceLauncher::SessionServiceLauncher(SessionServiceLaunch launch, std::string fingerprint)
    : launch_{std::move(launch)}, fingerprint_{std::move(fingerprint)} {
    if (launch_.columns == 0 || launch_.rows == 0 || launch_.columns > 1024 || launch_.rows > 1024)
        failed("Supervisor service launch has an invalid terminal size");
    if (launch_.service_program.isEmpty() || !QFileInfo{launch_.service_program}.isAbsolute() ||
        launch_.agent_program.isEmpty() || launch_.working_directory.isEmpty())
        failed("Supervisor service launch requires absolute programs and a working directory");
    try {
        // The service computes its fingerprint only after validate_launch
        // resolves the executable and canonicalizes the working directory.
        // Canonicalize this payload identically before admission and launch.
        const auto validated =
            lapis::session::validate_launch({.program = launch_.agent_program,
                                             .arguments = launch_.agent_arguments,
                                             .directory = launch_.working_directory,
                                             .size = {static_cast<std::uint16_t>(launch_.columns),
                                                      static_cast<std::uint16_t>(launch_.rows)},
                                             .agent = launch_.codex    ? AgentMode::codex
                                                      : launch_.claude ? AgentMode::claude
                                                                       : AgentMode::terminal});
        launch_.working_directory = validated.directory;
        launch_.agent_program = validated.program;
    } catch (const std::exception& error) {
        failed((std::string{"Invalid supervisor service child launch: "} + error.what()).c_str());
    }
    if (!hex(fingerprint_, fingerprint_hex_bytes) ||
        fingerprint_ != QString::fromLatin1(expected_fingerprint(launch_).toHex()).toStdString())
        failed("Supervisor service launch fingerprint does not match its payload");
    if (launch_.codex && launch_.claude)
        failed("Supervisor service launch accepts at most one agent integration mode");
}

QStringList SessionServiceLauncher::service_arguments(const DesiredSession& session) const {
    QStringList result;
    result << QStringLiteral("--session-id") << QString::fromStdString(session.identity.session_id)
           << QStringLiteral("--session-epoch") << QString::fromStdString(session.identity.epoch);
    if (launch_.codex)
        result << QStringLiteral("--codex");
    if (launch_.claude)
        result << QStringLiteral("--claude");
    result << QStringLiteral("--size")
           << QStringLiteral("%1x%2").arg(launch_.columns).arg(launch_.rows);
    result << QString::fromStdString(session.endpoint) << launch_.working_directory
           << launch_.agent_program;
    result << launch_.agent_arguments;
    return result;
}

bool SessionServiceLauncher::handshake(const DesiredSession& session, pid_t child_pid) {
    // A Codex service cannot answer the join until its own backend accepts a
    // connection; session_service.cpp waits up to about ten seconds to observe
    // that startup. Share one bounded budget across every attempt instead of
    // paying a full read timeout per retry, and pause between attempts so a
    // rejecting or not-yet-ready service is never polled in a tight loop.
    constexpr auto handshake_budget = std::chrono::seconds(20);
    const auto budget_end = std::chrono::steady_clock::now() + handshake_budget;
    while (std::chrono::steady_clock::now() < budget_end) {
        if (!process_alive(child_pid))
            return false;
        QLocalSocket socket;
        socket.connectToServer(socket_path(session), QLocalSocket::ReadWrite);
        if (socket.waitForConnected(20)) {
            const wire::AttachRequest request{
                .mode = wire::AttachMode::join,
                .fingerprint = QByteArray::fromHex(QByteArray::fromStdString(session.fingerprint)),
                .hyperlinks = true,
                .attention_phase = false,
                .paste_transactions = false,
            };
            const auto bytes = wire::frame(wire::Kind::attach, wire::encode_attach(request));
            if (socket.write(bytes) != bytes.size() || !socket.flush())
                return false;
            if (socket.bytesToWrite() != 0 && !socket.waitForBytesWritten(1000))
                return false;
            QByteArray buffer;
            bool retry_startup = false;
            while (!retry_startup && std::chrono::steady_clock::now() < budget_end) {
                // waitForReadyRead() can report false while a Unix-domain peer
                // has bytes buffered; drain what is available on every poll.
                buffer += socket.readAll();
                wire::Frame frame;
                while (wire::take_frame(buffer, frame)) {
                    if (frame.kind == wire::Kind::status) {
                        const auto status = wire::decode_status(frame.payload);
                        if (status.code == wire::StatusCode::overloaded) {
                            // The local listener accepts attachments before the
                            // PTY emits started. That transient admission window
                            // is retryable; identity and launch rejection is not.
                            socket.abort();
                            retry_startup = true;
                            break;
                        }
                        qWarning().noquote()
                            << "Supervisor service handshake rejected:" << status.message;
                        return false;
                    }
                    if (frame.kind != wire::Kind::hello) {
                        return false;
                    }
                    const auto hello = wire::decode_hello(frame.payload);
                    if (hello.attachment.identity != wire_identity(session)) {
                        const auto received = hello.attachment.identity;
                        const auto expected = wire_identity(session);
                        qWarning().noquote()
                            << "Supervisor service handshake identity mismatch: expected"
                            << QString::fromLatin1(expected.session_id.toHex()) << "/"
                            << QString::fromLatin1(expected.epoch.toHex()) << "received"
                            << QString::fromLatin1(received.session_id.toHex()) << "/"
                            << QString::fromLatin1(received.epoch.toHex());
                    } else if (hello.pid == 0) {
                        qWarning() << "Supervisor service handshake reported no terminal child";
                    }
                    if (hello.attachment.identity != wire_identity(session) || hello.pid == 0)
                        return false;
                    return true;
                }
                // A startup-overload retry must not keep polling the aborted
                // socket until the budget expires; leave for the next attempt.
                if (!retry_startup && !socket.waitForReadyRead(50))
                    continue;
            }
            if (retry_startup) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
            return false;
        }
        socket.abort();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    qWarning().noquote() << "Supervisor service handshake deadline expired:"
                         << socket_path(session);
    return false;
}

std::optional<ChildProcess> SessionServiceLauncher::adopt(const DesiredSession& session,
                                                          const std::string& spawn_token) {
    const auto endpoint = socket_path(session).toStdString();
    validate_parent(QString::fromStdString(endpoint));
    const auto owned = read_ownership(owner_path(endpoint), spawn_token);
    if (!owned)
        return std::nullopt;
    if (!process_alive(*owned) || !process_runs_program(*owned, launch_.service_program)) {
        remove_quietly(owner_path(endpoint));
        return std::nullopt;
    }
    if (!handshake(session, *owned)) {
        // The peer is alive and verified against this spawn token, so it is
        // recoverable: deleting its ownership record would orphan the running
        // service while no supervisor could adopt or stop it again. Keep the
        // record and let a later adoption retry or explicit stop decide.
        return std::nullopt;
    }
    return ChildProcess{*owned, spawn_token, endpoint, -1, true};
}

ChildProcess SessionServiceLauncher::launch(const DesiredSession& session,
                                            const std::string& spawn_token) {
    const auto endpoint = socket_path(session).toStdString();
    validate_parent(QString::fromStdString(endpoint));
    if (read_ownership(owner_path(endpoint), spawn_token))
        failed("A supervisor service already owns this spawn token");
    if (QFile::exists(QString::fromStdString(endpoint))) {
        QLocalSocket probe;
        probe.connectToServer(QString::fromStdString(endpoint), QLocalSocket::ReadWrite);
        if (probe.waitForConnected(100))
            failed("Supervisor service endpoint is already live");
        remove_quietly(endpoint);
    }
    remove_quietly(endpoint + ".log");

    const auto owner = owner_path(endpoint);
    const auto temporary = owner + "." + std::to_string(::getpid()) + ".tmp";
    const auto log_path = endpoint + ".log";
    remove_quietly(temporary);
    int barrier[2]{};
    if (::pipe(barrier) != 0)
        failed_system("Could not create supervisor service barrier");
    UniqueFd read_barrier{barrier[0]};
    UniqueFd write_barrier{barrier[1]};
    for (const int descriptor : barrier) {
        const int flags = ::fcntl(descriptor, F_GETFL);
        const int descriptor_flags = ::fcntl(descriptor, F_GETFD);
        if (flags < 0 || descriptor_flags < 0 ||
            ::fcntl(descriptor, F_SETFD,
                    static_cast<int>(static_cast<unsigned>(descriptor_flags) |
                                     static_cast<unsigned>(FD_CLOEXEC))) != 0)
            failed_system("Could not protect supervisor service barrier");
    }

    const QStringList arguments = service_arguments(session);
    std::vector<std::string> storage;
    storage.reserve(static_cast<std::size_t>(arguments.size()) + 1);
    storage.push_back(launch_.service_program.toStdString());
    for (const auto& argument : arguments)
        storage.push_back(argument.toStdString());
    std::vector<char*> argv;
    argv.reserve(storage.size() + 1);
    for (const auto& value : storage)
        argv.push_back(const_cast<char*>(value.c_str()));
    argv.push_back(nullptr);

    const pid_t pid = ::fork();
    if (pid < 0)
        failed_system("Could not fork supervisor service");
    if (pid == 0) {
        read_barrier.reset();
        if (::setsid() < 0)
            _exit(126);
        char ownership[96] = "LAPIS-SUP-OWNER-1\n";
        if (spawn_token.size() != spawn_token_hex_bytes)
            _exit(125);
        std::memcpy(ownership + 18, spawn_token.data(), spawn_token.size());
        std::size_t size = 18 + spawn_token.size();
        ownership[size++] = '\n';
        auto service_pid = static_cast<std::uint64_t>(::getpid());
        std::array<char, 24> digits{};
        std::size_t digit_count = 0;
        do {
            digits[digit_count++] = static_cast<char>('0' + service_pid % 10);
            service_pid /= 10;
        } while (service_pid > 0 && digit_count < digits.size());
        if (size + digit_count < sizeof(ownership)) {
            for (std::size_t index = 0; index < digit_count; ++index)
                ownership[size + index] = digits[digit_count - index - 1];
            size += digit_count;
            const int descriptor = ::open(
                temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
            if (descriptor >= 0) {
                std::size_t done = 0;
                while (done < size) {
                    const auto count = ::write(descriptor, ownership + done, size - done);
                    if (count < 0 && errno == EINTR)
                        continue;
                    if (count <= 0)
                        break;
                    done += static_cast<std::size_t>(count);
                }
                const bool complete = done == size && ::fsync(descriptor) == 0;
                static_cast<void>(::close(descriptor));
                if (!complete)
                    _exit(122);
                if (::rename(temporary.c_str(), owner.c_str()) != 0)
                    _exit(121);
                const char ready = 'r';
                if (::write(write_barrier.get(), &ready, 1) != 1)
                    _exit(120);
            } else {
                const int open_error = errno;
                _exit(open_error == EEXIST ? 124 : 123);
            }
        } else
            _exit(117);
        const UniqueFd null_input{::open("/dev/null", O_RDONLY | O_CLOEXEC)};
        const UniqueFd null_output{::open("/dev/null", O_WRONLY | O_CLOEXEC)};
        // Every post-fork path uses only materialized strings; allocating here
        // could deadlock the child against a parent allocator lock.
        const UniqueFd service_log{
            ::open(log_path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600)};
        if (!null_input || !null_output || !service_log ||
            ::dup2(null_input.get(), STDIN_FILENO) < 0 ||
            ::dup2(null_output.get(), STDOUT_FILENO) < 0 ||
            ::dup2(service_log.get(), STDERR_FILENO) < 0)
            _exit(126);
        ::execv(storage.front().c_str(), argv.data());
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
            const auto waited = ::waitpid(pid, &status, 0);
            remove_quietly(temporary);
            const std::string detail =
                waited == pid && WIFEXITED(status)
                    ? " (child exit " + std::to_string(WEXITSTATUS(status)) + ")"
                : waited == pid && WIFSIGNALED(status)
                    ? " (child signal " + std::to_string(WTERMSIG(status)) + ")"
                    : "";
            failed(("Supervisor service failed its ownership handshake" + detail).c_str());
        }
    }
    std::optional<pid_t> owned;
    try {
        owned = read_ownership(owner, spawn_token);
    } catch (...) {
        // The service already runs; an unreadable or mismatched record must
        // not leak it unreaped and unowned. Send the same cleanup as an
        // explicit mismatch before rethrowing.
        static_cast<void>(::killpg(pid, SIGKILL));
        static_cast<void>(wait_terminated(pid));
        remove_quietly(owner);
        throw;
    }
    if (!owned || *owned != pid) {
        static_cast<void>(::killpg(pid, SIGKILL));
        static_cast<void>(wait_terminated(pid));
        remove_quietly(owner);
        failed("Supervisor service ownership does not match the launched PID");
    }
    if (!handshake(session, pid)) {
        QFile service_log{QString::fromStdString(log_path)};
        if (service_log.open(QIODevice::ReadOnly))
            qWarning().noquote() << "Supervisor service log:"
                                 << QString::fromUtf8(service_log.readAll());
        static_cast<void>(::killpg(pid, SIGTERM));
        static_cast<void>(wait_terminated(pid));
        remove_quietly(owner);
        failed("Supervisor service did not complete its protocol handshake");
    }
    return ChildProcess{pid, spawn_token, endpoint, -1, false};
}

bool SessionServiceLauncher::alive(const ChildProcess& child) {
    if (child.pid <= 0)
        return false;
    // A crashed service this supervisor forked remains a zombie until reaped,
    // and kill(pid, 0) keeps succeeding for zombies. Reap own children here so
    // convergence observes the death and the restart path can fire; adopted
    // PIDs belong to another parent and can only be probed with signals.
    if (!child.adopted) {
        int status = 0;
        if (::waitpid(child.pid, &status, WNOHANG) == child.pid)
            return false;
    }
    return process_alive(child.pid);
}

bool SessionServiceLauncher::terminate(const ChildProcess& child) {
    if (child.pid <= 0)
        return true;
    // The PID was validated when the child was adopted or launched, but it may
    // have exited and been recycled since. Never signal a process or group that
    // no longer provably runs this exact service program.
    if (!process_runs_program(child.pid, launch_.service_program)) {
        if (!child.adopted) {
            int status = 0;
            static_cast<void>(::waitpid(child.pid, &status, WNOHANG));
        }
        return !process_alive(child.pid);
    }
    const auto signal_peer = [pid = child.pid](int signal_value) {
        static_cast<void>(::killpg(pid, signal_value));
    };
    signal_peer(SIGTERM);
    if (wait_terminated(child.pid))
        return true;
    signal_peer(SIGKILL);
    return wait_terminated(child.pid);
}

void SessionServiceLauncher::release(const ChildProcess& child) {
    if (!child.endpoint.empty())
        remove_quietly(owner_path(child.endpoint));
}

} // namespace lapis::supervisor
