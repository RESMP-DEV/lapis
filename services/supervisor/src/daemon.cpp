#include "lapis/supervisor/birth.hpp"
#include "lapis/supervisor/control_server.hpp"
#include "lapis/supervisor/session_service_launcher.hpp"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>

#include <csignal>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <unistd.h>
#include <utility>

namespace {
using namespace lapis::supervisor;

volatile std::sig_atomic_t stop_requested = 0;

void request_stop(int) { stop_requested = 1; }

std::string option(int argc, char** argv, std::string_view name) {
    const std::string prefix{std::string{name} + "="};
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        if (argument.starts_with(prefix))
            return std::string{argument.substr(prefix.size())};
    }
    throw std::runtime_error("Missing required daemon option: " + std::string{name});
}

std::string random_hex(std::size_t bytes) {
    std::string result;
    while (result.size() < bytes * 2) {
        const auto random = QUuid::createUuid().toRfc4122().toHex();
        result += std::string{random.constData(), static_cast<std::size_t>(random.size())};
    }
    return result.substr(0, bytes * 2);
}

class RandomIdentity final : public IdentityProvider {
  public:
    [[nodiscard]] std::string instance_epoch() override { return random_hex(epoch_hex_bytes / 2); }
    [[nodiscard]] std::string session_id() override { return random_hex(identity_hex_bytes / 2); }
    [[nodiscard]] std::string session_epoch() override {
        return random_hex(identity_hex_bytes / 2);
    }
    [[nodiscard]] std::string spawn_token() override {
        return random_hex(spawn_token_hex_bytes / 2);
    }
};

struct ParsedArguments {
    std::string state_directory;
    std::string control_endpoint;
};

ParsedArguments parse_arguments(int argc, char** argv) {
    if (argc != 3)
        throw std::runtime_error("Usage: lapis_supervisor_daemon --state-directory=DIR "
                                 "--control-endpoint=SOCKET");
    ParsedArguments result{option(argc, argv, "--state-directory"),
                           option(argc, argv, "--control-endpoint")};
    if (result.state_directory.empty() || result.state_directory.front() != '/' ||
        result.control_endpoint.empty() || result.control_endpoint.front() != '/')
        throw std::runtime_error("Supervisor daemon paths must be absolute");
    return result;
}

std::string internal_stop_request(const SupervisorRuntime& runtime) {
    const auto& state = runtime.state();
    if (!state.session)
        throw std::runtime_error("No active supervisor session to stop");
    const auto bytes = QJsonDocument{
        QJsonObject{
            {QStringLiteral("version"), 1},
            {QStringLiteral("action"), QStringLiteral("stop")},
            {QStringLiteral("supervisor_epoch"), QString::fromStdString(state.instance_epoch)},
            {QStringLiteral("token"), QString::fromStdString(state.session->spawn_token)},
            {QStringLiteral("session"), QJsonValue::Null},
        }}.toJson(QJsonDocument::Compact);
    return {bytes.constData(), static_cast<std::size_t>(bytes.size())};
}

void stop_runtime(const std::unique_ptr<SupervisorRuntime>& runtime) noexcept {
    if (!runtime)
        return;
    try {
        static_cast<void>(runtime->control(::geteuid(), internal_stop_request(*runtime)));
        static_cast<void>(runtime->converge());
    } catch (const std::exception&) { // NOLINT(bugprone-empty-catch) -- shutdown must not
        // displace the original daemon failure or throw from this noexcept path.
    }
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication application{argc, argv};
    std::unique_ptr<SupervisorRuntime> runtime;
    try {
        const auto arguments = parse_arguments(argc, argv);
        auto storage = std::make_shared<PosixStateStorage>(arguments.state_directory);
        auto registry = std::make_shared<SupervisorRegistry>(
            storage, std::make_shared<JsonStateCodec>(), std::make_shared<RandomIdentity>());
        if (registry->state().session)
            throw std::runtime_error("Supervisor daemon restart reconstruction is not implemented");

        std::signal(SIGINT, request_stop);
        std::signal(SIGTERM, request_stop);
        LocalControlServer server{
            arguments.control_endpoint, [&](uid_t peer_uid, const std::string& request) {
                if (peer_uid != ::geteuid())
                    return encode_birth_rejected();
                if (runtime)
                    return encode_birth_rejected();
                if (registry->state().session)
                    return encode_birth_rejected();
                try {
                    const auto birth = decode_birth_request(request);
                    SessionServiceLaunch launch;
                    launch.service_program = QString::fromLatin1(LAPIS_SUPERVISOR_SESSION_SERVICE);
                    launch.agent_program = birth.agent.program;
                    launch.agent_arguments = birth.agent.arguments;
                    launch.working_directory = birth.agent.working_directory;
                    launch.columns = birth.agent.columns;
                    launch.rows = birth.agent.rows;
                    launch.codex = birth.agent.mode == BirthAgentMode::codex;
                    launch.claude = birth.agent.mode == BirthAgentMode::claude;
                    auto next_launcher =
                        std::make_unique<SessionServiceLauncher>(launch, birth.fingerprint);
                    auto next_runtime =
                        std::make_unique<SupervisorRuntime>(registry, std::move(next_launcher));
                    DesiredSession desired;
                    desired.endpoint = birth.endpoint;
                    desired.fingerprint = birth.fingerprint;
                    const auto born = next_runtime->bootstrap(desired);
                    if (!born.session)
                        throw std::runtime_error("Supervisor birth produced no session");
                    runtime = std::move(next_runtime);
                    return encode_birth_reply(born.session->identity);
                } catch (const std::exception&) { // NOLINT(bugprone-empty-catch) -- the
                    // bounded protocol reply is rejection, never internal diagnostics.
                    return encode_birth_rejected();
                }
            }};

        while (!stop_requested) {
            server.poll_once(100);
        }
        stop_runtime(runtime);
        return 0;
    } catch (const std::exception& error) {
        stop_runtime(runtime);
        std::cerr << "lapis_supervisor_daemon: " << error.what() << '\n';
        return 1;
    }
}
