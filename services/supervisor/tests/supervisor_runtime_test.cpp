#include "lapis/supervisor/control_server.hpp"
#include "lapis/supervisor/runtime.hpp"

#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <csignal>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <poll.h>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using namespace lapis::supervisor;

void require(bool value, const char* message,
             std::source_location where = std::source_location::current()) {
    if (!value)
        throw std::runtime_error(std::string{"Supervisor runtime check failed at line "} +
                                 std::to_string(where.line()) + ": " + message);
}

std::string repeat(char value, std::size_t length) { return std::string(length, value); }

DesiredSession record(const std::string& endpoint = "/tmp/lapis-supervisor-runtime.sock") {
    DesiredSession result;
    result.endpoint = endpoint;
    result.fingerprint = repeat('a', fingerprint_hex_bytes);
    result.identity = {repeat('1', identity_hex_bytes), repeat('2', identity_hex_bytes)};
    return result;
}

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

class FakeClock final : public MonotonicClock {
  public:
    [[nodiscard]] std::uint64_t nanoseconds() override { return now; }
    std::uint64_t now{1'000'000};
};

struct FakeChild {
    pid_t pid{};
    bool alive{true};
};

class FakeLauncherWorld {
  public:
    [[nodiscard]] std::optional<ChildProcess> adopt(const DesiredSession& session,
                                                    const std::string& token) {
        if (decline_adoption)
            return std::nullopt;
        const auto found = children.find(token);
        if (found == children.end() || !found->second.alive)
            return std::nullopt;
        return ChildProcess{found->second.pid, token, session.endpoint, -1, true, "", "", ""};
    }

    [[nodiscard]] ChildProcess launch(const DesiredSession& session, const std::string& token) {
        require(!children.contains(token), "a fake token is launched once");
        const pid_t pid = ++next_pid;
        children.emplace(token, FakeChild{pid, true});
        ++launches;
        return ChildProcess{pid, token, session.endpoint, -1, false, "", "", ""};
    }

    [[nodiscard]] bool alive(const ChildProcess& child) {
        const auto found = children.find(child.spawn_token);
        return found != children.end() && found->second.pid == child.pid && found->second.alive;
    }

    [[nodiscard]] bool terminate(const ChildProcess& child) {
        const auto found = children.find(child.spawn_token);
        if (found == children.end())
            return true;
        found->second.alive = false;
        ++terminations;
        return true;
    }

    void release(const ChildProcess&) {}

    [[nodiscard]] bool peer_preserved(const DesiredSession& session, const std::string& token) {
        const auto found = children.find(token);
        return found != children.end() && found->second.alive && session.spawn_token == token;
    }

    void kill_all() {
        for (auto& [token, child] : children) {
            static_cast<void>(token);
            child.alive = false;
        }
    }

    unsigned launches{0};
    unsigned terminations{0};
    bool decline_adoption{false};
    pid_t next_pid{1000};
    std::vector<std::string> launch_order;
    std::vector<std::string> children_keys() const {
        std::vector<std::string> keys;
        for (const auto& [key, value] : children) {
            static_cast<void>(key);
            static_cast<void>(value);
            keys.push_back(key);
        }
        return keys;
    }

  private:
    std::map<std::string, FakeChild> children;
};

class FakeLauncher final : public ChildLauncher {
  public:
    explicit FakeLauncher(std::shared_ptr<FakeLauncherWorld> world) : world_{std::move(world)} {}
    [[nodiscard]] std::optional<ChildProcess> adopt(const DesiredSession& session,
                                                    const std::string& token) override {
        return world_->adopt(session, token);
    }
    [[nodiscard]] bool peer_preserved(const DesiredSession& session,
                                      const std::string& token) override {
        return world_->peer_preserved(session, token);
    }
    [[nodiscard]] ChildProcess launch(const DesiredSession& session,
                                      const std::string& token) override {
        world_->launch_order.push_back(token);
        return world_->launch(session, token);
    }
    [[nodiscard]] bool alive(const ChildProcess& child) override { return world_->alive(child); }
    [[nodiscard]] bool terminate(const ChildProcess& child) override {
        return world_->terminate(child);
    }
    void release(const ChildProcess& child) override { world_->release(child); }

  private:
    std::shared_ptr<FakeLauncherWorld> world_;
};

std::shared_ptr<SupervisorRegistry> registry(std::shared_ptr<StateStorage> storage) {
    return std::make_shared<SupervisorRegistry>(std::move(storage),
                                                std::make_shared<JsonStateCodec>(),
                                                std::make_shared<DeterministicIdentity>());
}

std::string json(const QJsonObject& object) {
    const auto bytes = QJsonDocument{object}.toJson(QJsonDocument::Compact);
    return {bytes.constData(), static_cast<std::size_t>(bytes.size())};
}

std::string stop_request(const std::string& token, const std::string& epoch = repeat('3', 32)) {
    return json(QJsonObject{
        {QStringLiteral("version"), 1},
        {QStringLiteral("action"), QStringLiteral("stop")},
        {QStringLiteral("supervisor_epoch"), QString::fromStdString(epoch)},
        {QStringLiteral("token"), QString::fromStdString(token)},
        {QStringLiteral("session"), QJsonValue::Null},
    });
}

std::string start_request(const std::string& token, const std::string& endpoint) {
    const auto session = record(endpoint);
    return json(QJsonObject{
        {QStringLiteral("version"), 1},
        {QStringLiteral("action"), QStringLiteral("start")},
        {QStringLiteral("supervisor_epoch"), QString::fromStdString(repeat('3', 32))},
        {QStringLiteral("token"), QString::fromStdString(token)},
        {QStringLiteral("session"),
         QJsonObject{{QStringLiteral("endpoint"), QString::fromStdString(session.endpoint)},
                     {QStringLiteral("fingerprint"), QString::fromStdString(session.fingerprint)},
                     {QStringLiteral("identity"),
                      QJsonObject{{QStringLiteral("session_id"),
                                   QString::fromStdString(session.identity.session_id)},
                                  {QStringLiteral("epoch"),
                                   QString::fromStdString(session.identity.epoch)}}}}},
    });
}

std::string disable_request(const std::string& token) {
    return json(QJsonObject{
        {QStringLiteral("version"), 1},
        {QStringLiteral("action"), QStringLiteral("disable")},
        {QStringLiteral("supervisor_epoch"), QString::fromStdString(repeat('3', 32))},
        {QStringLiteral("token"), QString::fromStdString(token)},
        {QStringLiteral("session"), QJsonValue::Null},
    });
}

std::string control_status(const std::string& bytes) {
    const auto document = QJsonDocument::fromJson(QByteArray::fromStdString(bytes));
    require(document.isObject(), "control reply is an object");
    const auto status = document.object().value(QStringLiteral("status")).toString();
    return status.toStdString();
}

void authenticated_control_and_stop() {
    auto world = std::make_shared<FakeLauncherWorld>();
    auto owner = registry(std::make_shared<MemoryStateStorage>());
    SupervisorRuntime runtime{owner, std::make_shared<FakeLauncher>(world)};
    const auto bootstrapped = runtime.bootstrap(record());
    require(world->launches == 1 && bootstrapped.enabled, "bootstrap launches one child");

    const uid_t foreign_uid = ::geteuid() == 0 ? 1 : 0;
    const auto unauthorized =
        runtime.control(foreign_uid, stop_request(bootstrapped.session->spawn_token));
    require(control_status(unauthorized) == "unauthorized" && world->launches == 1,
            "a foreign peer UID cannot control the supervisor");
    require(control_status(runtime.control(::geteuid(), "{")) == "rejected",
            "malformed control is rejected");
    require(control_status(runtime.control(::geteuid(), std::string(max_control_bytes + 1, ' '))) ==
                "rejected",
            "oversized control is rejected");

    const auto applied =
        runtime.control(::geteuid(), stop_request(bootstrapped.session->spawn_token));
    require(control_status(applied) == "applied" && world->terminations == 1 &&
                runtime.state().session->desired_state == DesiredState::stopped,
            "authenticated stop terminates the one child");
    require(runtime.converge() == Convergence::stopped && world->launches == 1,
            "an explicitly stopped session does not restart");
    const auto restarted =
        runtime.control(::geteuid(), start_request(runtime.state().session->spawn_token,
                                                   runtime.state().session->endpoint));
    require(control_status(restarted) == "applied" && world->launches == 2 &&
                runtime.state().session->desired_state == DesiredState::started,
            "authenticated start replaces the stopped slot");
}

void restarted_runtime_adopts_without_duplicate() {
    auto world = std::make_shared<FakeLauncherWorld>();
    auto storage = std::make_shared<MemoryStateStorage>();
    const auto owner = registry(storage);
    const auto saved = [&] {
        SupervisorRuntime runtime{owner, std::make_shared<FakeLauncher>(world)};
        return runtime.bootstrap(record());
    }();
    require(world->launches == 1, "the first supervisor starts exactly once");

    SupervisorRuntime restarted{owner, std::make_shared<FakeLauncher>(world)};
    require(restarted.converge() == Convergence::converged && world->launches == 1,
            "restart adopts the child recorded by the spawn token");
    require(restarted.state() == saved, "adopted state is unchanged");
    const auto stopped = restarted.control(::geteuid(), stop_request(saved.session->spawn_token));
    require(control_status(stopped) == "applied" && world->terminations == 1,
            "the restarted supervisor can stop its adopted child");
}

void preserved_peer_failure_retains_identity_for_retry() {
    auto world = std::make_shared<FakeLauncherWorld>();
    const auto owner = registry(std::make_shared<MemoryStateStorage>());
    const auto saved = [&] {
        SupervisorRuntime runtime{owner, std::make_shared<FakeLauncher>(world)};
        return runtime.bootstrap(record());
    }();
    world->decline_adoption = true;
    {
        SupervisorRuntime blocked{owner, std::make_shared<FakeLauncher>(world)};
        bool refused = false;
        try {
            static_cast<void>(blocked.converge());
        } catch (const std::runtime_error&) {
            refused = true;
        }
        require(refused && world->launches == 1 && blocked.state() == saved,
                "a preserved live peer neither rotates nor launches a replacement");
    }

    world->decline_adoption = false;
    SupervisorRuntime retry{owner, std::make_shared<FakeLauncher>(world)};
    require(retry.converge() == Convergence::converged && world->launches == 1 &&
                retry.state() == saved,
            "the preserved peer can be adopted once verification recovers");
}

void bounded_restart_admission() {
    auto world = std::make_shared<FakeLauncherWorld>();
    auto clock = std::make_shared<FakeClock>();
    auto owner = registry(std::make_shared<MemoryStateStorage>());
    SupervisorRuntime runtime{owner, std::make_shared<FakeLauncher>(world), clock};
    const auto saved = runtime.bootstrap(record());

    require(world->launches == 1, "convergence keeps a live child");
    // Each converge call observes the newest fake child dead. The initial
    // admission is not a crash restart; the next three are admitted.
    world->kill_all();
    static_cast<void>(runtime.converge());
    world->kill_all();
    static_cast<void>(runtime.converge());
    world->kill_all();
    static_cast<void>(runtime.converge());
    world->kill_all();
    const auto refused = runtime.converge();
    require(world->launches == 4, "three restarts are admitted");
    require(runtime.state().session->identity.epoch != saved.session->identity.epoch,
            "a crash restart rotates the service epoch");
    require(refused == Convergence::exhausted && world->launches == 4,
            "the fourth restart is refused");
    require(runtime.state().session->desired_state == DesiredState::stopped &&
                runtime.state().session->blocked_reason == "restart admission exhausted",
            "exhaustion is explicit persisted state");
    require(runtime.converge() == Convergence::stopped, "exhaustion does not retry silently");
    static_cast<void>(saved.session);
}

void explicit_disable() {
    auto world = std::make_shared<FakeLauncherWorld>();
    auto owner = registry(std::make_shared<MemoryStateStorage>());
    SupervisorRuntime runtime{owner, std::make_shared<FakeLauncher>(world)};
    const auto saved = runtime.bootstrap(record());
    const auto reply = runtime.control(::geteuid(), disable_request(saved.session->spawn_token));
    require(control_status(reply) == "applied" && world->terminations == 1,
            "disable terminates the child");
    require(!runtime.state().enabled &&
                runtime.state().session->blocked_reason == "supervisor disabled",
            "disable is distinct from stop");
    require(runtime.converge() == Convergence::disabled && world->launches == 1,
            "a disabled supervisor does not launch");
}

void write_frame(int descriptor, const std::string& payload) {
    std::array<char, 4> header{};
    const auto length = static_cast<uint32_t>(payload.size());
    header[0] = static_cast<char>((length >> 24U) & 0xffU);
    header[1] = static_cast<char>((length >> 16U) & 0xffU);
    header[2] = static_cast<char>((length >> 8U) & 0xffU);
    header[3] = static_cast<char>(length & 0xffU);
    require(::write(descriptor, header.data(), header.size()) == 4, "write frame header");
    if (!payload.empty())
        require(::write(descriptor, payload.data(), payload.size()) ==
                    static_cast<ssize_t>(payload.size()),
                "write frame payload");
}

std::string receive_reply(int descriptor) {
    std::array<char, 128> bytes{};
    pollfd wait{descriptor, POLLIN, 0};
    require(::poll(&wait, 1, 2000) == 1, "control reply arrives");
    const auto count = ::read(descriptor, bytes.data(), bytes.size());
    require(count > 0, "control reply is nonempty");
    return std::string{bytes.data(), static_cast<std::size_t>(count)};
}

void strict_local_server() {
    auto world = std::make_shared<FakeLauncherWorld>();
    auto owner = registry(std::make_shared<MemoryStateStorage>());
    QTemporaryDir directory;
    require(directory.isValid(), "control test directory");
    const auto endpoint = directory.filePath(QStringLiteral("control.sock")).toStdString();
    SupervisorRuntime runtime{owner, std::make_shared<FakeLauncher>(world)};
    const auto saved = runtime.bootstrap(record());
    std::optional<LocalControlServer> server;
    try {
        server.emplace(endpoint, [&runtime](uid_t uid, const std::string& request) {
            return runtime.control(uid, request);
        });
    } catch (const std::exception& error) {
        // This managed sandbox denies AF_UNIX bind(2), including inside the
        // workspace. Direct runtime.control cases still cover peer-UID policy;
        // actual socket framing remains a separate host qualification check.
        if (std::string_view{error.what()}.find("Operation not permitted") !=
            std::string_view::npos) {
            std::cout << "supervisor-runtime: control socket bind unavailable\n";
            return;
        }
        throw;
    }

    int client = ::socket(AF_UNIX, SOCK_STREAM, 0);
    require(client >= 0, "create control client");
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    require(endpoint.size() < sizeof(address.sun_path), "control endpoint length");
    std::memcpy(address.sun_path, endpoint.c_str(), endpoint.size() + 1);
    require(::connect(client, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0,
            "connect local control");
    const std::array<char, 4> oversized{{static_cast<char>(0xff), static_cast<char>(0xff),
                                         static_cast<char>(0xff), static_cast<char>(0xff)}};
    require(::write(client, oversized.data(), oversized.size()) == 4, "send oversized header");
    server->poll_once(1000);
    char ignored = 0;
    pollfd wait{client, POLLIN, 0};
    require(::poll(&wait, 1, 100) == 1 && ::read(client, &ignored, 1) == 0,
            "an oversized frame closes the client");
    static_cast<void>(::close(client));

    client = ::socket(AF_UNIX, SOCK_STREAM, 0);
    require(client >= 0, "create second control client");
    require(::connect(client, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0,
            "connect after rejection");
    const auto payload = stop_request(saved.session->spawn_token);
    write_frame(client, payload);
    server->poll_once(1000);
    require(control_status(receive_reply(client)) == "applied", "server applies bounded request");
    require(world->terminations == 1, "server controls the lifecycle");
    static_cast<void>(::close(client));
}

void harmless_child_ownership_fixture() {
    QTemporaryDir directory;
    require(directory.isValid(), "ownership test directory");
    const auto endpoint = directory.filePath(QStringLiteral("session.sock")).toStdString();
    auto world = std::make_shared<FakeLauncherWorld>();
    auto storage = std::make_shared<MemoryStateStorage>();
    auto owner = registry(storage);
    const std::string child = LAPIS_SUPERVISOR_CHILD_FIXTURE;
    pid_t first_pid = -1;
    {
        SupervisorRuntime runtime{
            owner, std::make_shared<OwnershipChildLauncher>(LaunchCommand{child, {}})};
        const auto saved = runtime.bootstrap(record(endpoint));
        require(runtime.converge() == Convergence::converged, "fixture starts");
        first_pid = runtime.child_pid();
        struct stat info{};
        const auto owned = endpoint + ".supervisor-owner";
        require(::lstat(owned.c_str(), &info) == 0 && S_ISREG(info.st_mode) &&
                    (info.st_mode & 07777U) == 0600U,
                "child writes a private ownership record");
        require(::kill(first_pid, 0) == 0, "harmless fixture remains alive");

        SupervisorRuntime restarted{
            owner, std::make_shared<OwnershipChildLauncher>(LaunchCommand{child, {}})};
        require(restarted.converge() == Convergence::converged,
                "the second supervisor adopts the live fixture");
        require(restarted.child_pid() == first_pid, "adoption preserves the child PID");
        require(::kill(first_pid, 0) == 0, "adoption does not duplicate or kill the child");
        const auto stop = restarted.control(::geteuid(), stop_request(saved.session->spawn_token));
        require(control_status(stop) == "applied", "authenticated stop controls fixture");
    }
    for (int attempt = 0; attempt < 300; ++attempt) {
        if (::kill(first_pid, 0) != 0) {
            int status = 0;
            static_cast<void>(::waitpid(first_pid, &status, WNOHANG));
            break;
        }
        timespec pause{0, 10'000'000};
        ::nanosleep(&pause, nullptr);
    }
    require(::kill(first_pid, 0) != 0 && errno == ESRCH, "fixture terminates");
}

} // namespace

int main() {
    try {
        authenticated_control_and_stop();
        restarted_runtime_adopts_without_duplicate();
        preserved_peer_failure_retains_identity_for_retry();
        bounded_restart_admission();
        explicit_disable();
        harmless_child_ownership_fixture();
        strict_local_server();
        std::cout << "supervisor-runtime: ok\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
